#include "core/media/audio_realtime.h"
#include "core/media/audio_ring.h"
#include "core/media/media_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <thread>
#include <SDL3/SDL.h>

namespace oa::media {
#if !defined(__EMSCRIPTEN__)
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kBlock = 256;
constexpr size_t kDecodeBlock = 1024;
constexpr size_t kDecodedFrames = 8192;

std::string key(const SoundChannel& ch) {
    return std::to_string(int(ch.category)) + "|" + ch.id;
}
const SoundChannel* find_channel(const AudioEngine::State& state, SoundCategory cat,
                                 const std::string& id) {
    if (cat == SoundCategory::Bgm) return state.bgm_channel ? &*state.bgm_channel : nullptr;
    const auto& channels = cat == SoundCategory::Se ? state.se_channels : state.voice_channels;
    const auto it = channels.find(id);
    return it == channels.end() ? nullptr : &it->second;
}
bool same_play(const SoundChannel& a, const SoundChannel& b) {
    return a.playback_id == b.playback_id && a.started_at_ms == b.started_at_ms &&
           a.file == b.file && a.loop_play == b.loop_play && a.loop_file == b.loop_file;
}
}

struct RealtimeAudio::Impl {
    struct Stream {
        explicit Stream(SoundChannel ch) : identity(ch), channel(std::move(ch)), pcm(kDecodedFrames * 2) {}
        const SoundChannel identity; // immutable metadata for loader/decode jobs
        SoundChannel channel;
        MediaPlayers::Player decoder; // loader initializes; one decode job owns it afterwards
        std::vector<uint8_t> loop_bytes;
        AudioRing<float> pcm;
        std::atomic<bool> ready{false}, eof{false}, cancel{false}, scheduled{false};
        bool started = false, reported = false; // mixer only
    };
    struct Done {
        SoundChannel channel;
        uint64_t epoch;
    };
    AudioSink& sink;
    Loader loader;
    std::atomic<bool> stopping{false}, paused{false};
#ifdef OA_USE_SDL2
    bool watch_registered = false;
    static int SDLCALL lifecycle(void* data, SDL_Event* event) {
        auto& self = *static_cast<Impl*>(data);
        if (event->type == SDL_APP_WILLENTERBACKGROUND || event->type == SDL_APP_DIDENTERBACKGROUND)
            self.set_paused(true);
        if (event->type == SDL_APP_DIDENTERFOREGROUND) self.set_paused(false);
        return 1;
    }
#endif
    std::thread mixer_thread, loader_thread;
    std::vector<std::thread> decode_threads;
    std::mutex control_mu, load_mu, decode_mu, done_mu;
    std::condition_variable control_cv, load_cv, decode_cv;
    AudioEngine::State pending, state;
    Clock::time_point pending_at = Clock::now(), state_at = pending_at;
    bool dirty = false;
    uint64_t control_serial = 0; // protected by control_mu, including worker notifications
    std::atomic<uint64_t> epoch{0};
    uint64_t mixer_epoch = 0;
    std::deque<std::shared_ptr<Stream>> loads, decodes;
    std::map<std::string, std::shared_ptr<Stream>> streams; // mixer owns membership
    std::vector<Done> done;
    std::atomic<uint64_t> mixed_frames{0}, short_frames{0}, max_load_us{0};
    std::atomic<uint64_t> mixer_waits{0};
    std::atomic<size_t> stream_count{0};
    const size_t target;
    bool can_mix(const Stream& stream) const {
        if (!stream.ready.load()) return false;
        const size_t frames = stream.pcm.available() / 2;
        // Starting on the very first decoded block lets the high-priority
        // mixer outrun the decoder while filling an empty device queue.
        return stream.started ? (frames > 0 || !stream.eof.load()) :
            (frames >= std::min(target, kDecodedFrames) || (frames > 0 && stream.eof.load()));
    }

    Impl(AudioSink& output, Loader read)
        : sink(output), loader(std::move(read)),
          target(std::max(2 * output.device_frames(), size_t(1024))) {
        try {
            loader_thread = std::thread([this] { load_main(); });
            for (int i = 0; i < 2; ++i) decode_threads.emplace_back([this] { decode_main(); });
            mixer_thread = std::thread([this] { mix_main(); });
#ifdef OA_USE_SDL2
            SDL_AddEventWatch(lifecycle, this);
            watch_registered = true;
#endif
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~Impl() { shutdown(); }
    void shutdown() {
#ifdef OA_USE_SDL2
        if (watch_registered) { SDL_DelEventWatch(lifecycle, this); watch_registered = false; }
#endif
        stopping.store(true, std::memory_order_release);
        wake_mixer();
        // Pair shutdown with each wait mutex as well as the atomic predicate:
        // otherwise a worker can enter wait after the final notification.
        { std::lock_guard<std::mutex> lock(load_mu); }
        load_cv.notify_all();
        { std::lock_guard<std::mutex> lock(decode_mu); }
        decode_cv.notify_all();
        if (mixer_thread.joinable()) mixer_thread.join();
        if (loader_thread.joinable()) loader_thread.join();
        for (auto& worker : decode_threads) if (worker.joinable()) worker.join();
    }
    void wake_mixer() {
        {
            std::lock_guard<std::mutex> lock(control_mu);
            ++control_serial;
        }
        control_cv.notify_one();
    }
    void set_paused(bool value) {
        if (paused.exchange(value) != value) wake_mixer();
    }
    void schedule(const std::shared_ptr<Stream>& stream) {
        if (stream->cancel.load() || stream->eof.load() || !stream->ready.load() ||
            stream->pcm.writable() < kDecodeBlock * 2 || stream->scheduled.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(decode_mu);
            decodes.push_back(stream);
        }
        decode_cv.notify_one();
    }
    void queue_load(const std::shared_ptr<Stream>& stream) {
        {
            std::lock_guard<std::mutex> lock(load_mu);
            loads.erase(std::remove_if(loads.begin(), loads.end(),
                [](const auto& old) { return old->cancel.load(); }), loads.end());
            loads.push_back(stream);
        }
        load_cv.notify_one();
    }
    void load_main() {
        while (!stopping.load()) {
            std::shared_ptr<Stream> stream;
            {
                std::unique_lock<std::mutex> lock(load_mu);
                load_cv.wait(lock, [&] { return stopping.load() || !loads.empty(); });
                if (stopping.load()) break;
                stream = std::move(loads.front()); loads.pop_front();
            }
            if (stream->cancel.load()) continue;
            const auto begin = Clock::now();
            try {
                auto& p = stream->decoder;
                p.file = stream->identity.file;
                p.loop_play = stream->identity.loop_play;
                p.loop_file = stream->identity.loop_file;
                if (loader) {
                    if (auto bytes = loader(p.file)) p.src = VorbisSource::open(std::move(*bytes));
                }
                if (p.src && !stream->cancel.load()) {
                    p.src_ch = std::max(1, p.src->channels());
                    p.avail_ch = p.src_ch > 2 ? 1 : p.src_ch;
                    p.rate = p.src->rate() > 0 ? p.src->rate() : MediaPlayers::kOutputRate;
                    // Read the B segment before streaming starts. Crossing the
                    // loop boundary must not send the decode worker into VFS I/O.
                    if (p.loop_file && loader) {
                        if (auto bytes = loader(*p.loop_file)) stream->loop_bytes = std::move(*bytes);
                    }
                } else {
                    stream->eof.store(true);
                }
            } catch (...) { stream->eof.store(true); }
            const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
            uint64_t previous = max_load_us.load();
            while (us > previous && !max_load_us.compare_exchange_weak(previous, us)) {}
            stream->ready.store(true, std::memory_order_release);
            schedule(stream);
            wake_mixer();
        }
    }
    void decode_main() {
        std::array<float, kDecodeBlock * 2> block{};
        while (!stopping.load()) {
            std::shared_ptr<Stream> stream;
            {
                std::unique_lock<std::mutex> lock(decode_mu);
                decode_cv.wait(lock, [&] { return stopping.load() || !decodes.empty(); });
                if (stopping.load()) break;
                stream = std::move(decodes.front()); decodes.pop_front();
            }
            bool wake = false;
            if (!stream->cancel.load()) {
                try {
                    const Loader loop = [stream](const std::string&) -> std::optional<std::vector<uint8_t>> {
                        if (stream->loop_bytes.empty()) return std::nullopt;
                        return std::move(stream->loop_bytes);
                    };
                    const size_t n = decode_audio_block(stream->decoder, loop, block.data(), kDecodeBlock);
                    const size_t before = stream->pcm.available() / 2;
                    stream->pcm.push(block.data(), n * 2);
                    const size_t startup = std::min(target, kDecodedFrames);
                    wake = (before < startup && before + n >= startup) || n < kDecodeBlock;
                    if (n < kDecodeBlock) stream->eof.store(true, std::memory_order_release);
                } catch (...) { stream->eof.store(true, std::memory_order_release); wake = true; }
            }
            stream->scheduled.store(false);
            schedule(stream);
            if (wake) wake_mixer();
        }
    }
    void apply_snapshot() {
        {
            std::lock_guard<std::mutex> lock(control_mu);
            if (!dirty) return;
            state = std::move(pending);
            state_at = pending_at;
            dirty = false;
            const uint64_t current = epoch.load();
            if (mixer_epoch != current) {
                for (auto& [id, stream] : streams) stream->cancel.store(true);
                streams.clear();
                sink.clear_mix();
                mixer_epoch = current;
            }
        }
        std::map<std::string, const SoundChannel*> active;
        auto add = [&](const SoundChannel& ch) { if (ch.playing) active.emplace(key(ch), &ch); };
        if (state.bgm_channel) add(*state.bgm_channel);
        for (const auto& [id, ch] : state.se_channels) add(ch);
        for (const auto& [id, ch] : state.voice_channels) add(ch);
        for (auto it = streams.begin(); it != streams.end();) {
            const auto found = active.find(it->first);
            if (found == active.end() || !same_play(it->second->channel, *found->second)) {
                it->second->cancel.store(true);
                it = streams.erase(it); // shared jobs retire without a gameplay-thread join
            } else {
                it->second->channel = *found->second;
                ++it;
            }
        }
        for (const auto& [id, ch] : active) if (streams.find(id) == streams.end()) {
            auto stream = std::make_shared<Stream>(*ch);
            streams.emplace(id, stream);
            queue_load(stream);
        }
        stream_count.store(streams.size());
    }
    void mix_main() {
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH); // best effort, without realtime scheduling privileges
        std::array<float, kBlock * 2> mix{}, source{};
        auto last_iteration = Clock::now();
        bool was_paused = false;
        while (!stopping.load()) {
            uint64_t observed_serial;
            {
                std::lock_guard<std::mutex> lock(control_mu);
                observed_serial = control_serial;
            }
            apply_snapshot();
            const auto current_time = Clock::now();
            const bool explicit_pause = paused.load();
            const bool device_paused = sink.paused();
            const bool is_paused = explicit_pause || device_paused;
            // Include the final paused interval on resume. A snapshot received
            // during suspension freezes only the time since its own anchor.
            if (was_paused) state_at += current_time - std::max(state_at, last_iteration);
            last_iteration = current_time;
            was_paused = is_paused;
            const size_t level = sink.queued_frames(AudioSink::kStreamMix);
            bool playable = false;
            for (const auto& [id, s] : streams)
                playable |= can_mix(*s);
            sink.set_mix_active(playable && !is_paused);
            if (!is_paused && playable && level < target) {
                const size_t frames = std::min(kBlock, target - level);
                std::fill(mix.begin(), mix.end(), 0.0f);
                const uint64_t now = state.clock_ms + uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state_at).count());
                for (auto& [id, s] : streams) {
                    if (!can_mix(*s)) continue;
                    const size_t n = s->pcm.pop(source.data(), frames * 2) / 2;
                    if (n) s->started = true;
                    if (s->started && n < frames && !s->eof.load()) short_frames.fetch_add(frames - n);
                    const auto& ch = s->channel;
                    const float category = ch.category == SoundCategory::Bgm ? state.bgm_volume :
                                           ch.category == SoundCategory::Se ? state.se_volume : state.voice_volume;
                    for (size_t f = 0; f < n; ++f) {
                        float gain = ch.current_gain, pan = ch.current_pan;
                        bool finished = false;
                        if (ch.fade) ch.fade->current_value(now + f * 1000 / MediaPlayers::kOutputRate, &gain, &pan, &finished);
                        gain *= category * state.master_volume;
                        mix[f * 2] += source[f * 2] * gain * (pan > 0 ? 1.0f - pan : 1.0f);
                        mix[f * 2 + 1] += source[f * 2 + 1] * gain * (pan < 0 ? 1.0f + pan : 1.0f);
                    }
                    schedule(s);
                }
                for (size_t i = 0; i < frames * 2; ++i) mix[i] = std::clamp(mix[i], -1.0f, 1.0f);
                sink.push(mix.data(), frames);
                capture_realtime_mix(mix.data(), frames, level,
                                     sink.queued_frames(AudioSink::kStreamMix));
                mixed_frames.fetch_add(frames);
                continue;
            }
            // Check EOF even if the only channel has drained (no playable
            // stream left). Events carry the play identity and reset epoch.
            for (auto& [id, s] : streams) {
                if (s->ready.load() && s->eof.load() && s->pcm.available() == 0 &&
                    !s->channel.loop_play && !s->reported) {
                    s->reported = true;
                    std::lock_guard<std::mutex> lock(done_mu);
                    done.push_back({s->channel, mixer_epoch});
                }
            }
            std::unique_lock<std::mutex> lock(control_mu);
            mixer_waits.fetch_add(1, std::memory_order_relaxed);
            const auto changed = [&] { return stopping.load() || dirty || control_serial != observed_serial; };
            if (explicit_pause || (!playable && !device_paused)) {
                control_cv.wait(lock, changed);
            } else if (device_paused) {
                // SDL may pause a device without an app lifecycle event. Keep
                // a slow fallback to notice that device becoming usable again.
                control_cv.wait_for(lock, std::chrono::milliseconds(50), changed);
            } else {
                // Leave one callback plus half a callback as scheduling margin.
                // No callback-side locks or notifications are needed.
                const size_t period = std::max(kBlock, sink.device_frames());
                const size_t reserve = period + period / 2;
                const size_t spare = level > reserve ? level - reserve : 0;
                const auto delay = std::chrono::microseconds(std::max<size_t>(1000,
                    spare * 1000000 / MediaPlayers::kOutputRate));
                control_cv.wait_for(lock, delay, changed);
            }
        }
        for (auto& [id, stream] : streams) stream->cancel.store(true);
        // This is a std::thread, so SDL's own thread trampoline will not
        // release the error/converter TLS created on the producer.
#ifdef OA_USE_SDL2
        SDL_TLSCleanup();
#else
        SDL_CleanupTLS();
#endif
    }
};

RealtimeAudio::RealtimeAudio(AudioSink& sink, Loader loader)
    : impl_(std::make_unique<Impl>(sink, std::move(loader))) {}
RealtimeAudio::~RealtimeAudio() = default;
void RealtimeAudio::submit(AudioEngine::State state) {
    {
        std::lock_guard<std::mutex> lock(impl_->control_mu);
        impl_->pending = std::move(state);
        impl_->pending_at = Clock::now();
        impl_->dirty = true;
    }
    impl_->control_cv.notify_one();
}
void RealtimeAudio::stop_all() {
    {
        std::lock_guard<std::mutex> lock(impl_->control_mu);
        impl_->epoch.fetch_add(1);
        impl_->pending = {};
        impl_->pending_at = Clock::now();
        impl_->dirty = true;
    }
    impl_->control_cv.notify_one();
}
void RealtimeAudio::poll(AudioEngine& engine, const MediaPlayers::FinishFn& finish) {
    std::vector<Impl::Done> done;
    {
        std::lock_guard<std::mutex> lock(impl_->done_mu);
        done.swap(impl_->done);
    }
    for (const auto& event : done) {
        const auto& ch = event.channel;
        const auto* current = find_channel(engine.state(), ch.category, ch.id);
        if (event.epoch == impl_->epoch.load() && current && current->playing &&
            current->playback_id == ch.playback_id && current->started_at_ms == ch.started_at_ms && finish)
            finish(ch.category, ch.category == SoundCategory::Bgm ? std::string() : ch.id);
    }
}
void RealtimeAudio::set_paused(bool paused) {
    impl_->set_paused(paused);
}
MediaPlayers::RealtimeStats RealtimeAudio::stats() const {
    return {impl_->mixed_frames.load(), impl_->short_frames.load(),
            impl_->max_load_us.load(), impl_->stream_count.load(), impl_->mixer_waits.load()};
}
size_t RealtimeAudio::target_frames() const { return impl_->target; }

#else
// Browser hosts keep the caller-clock path without native thread entry points.
struct RealtimeAudio::Impl {};
RealtimeAudio::RealtimeAudio(AudioSink&, Loader) : impl_(std::make_unique<Impl>()) {}
RealtimeAudio::~RealtimeAudio() = default;
void RealtimeAudio::submit(AudioEngine::State) {}
void RealtimeAudio::stop_all() {}
void RealtimeAudio::set_paused(bool) {}
void RealtimeAudio::poll(AudioEngine&, const MediaPlayers::FinishFn&) {}
MediaPlayers::RealtimeStats RealtimeAudio::stats() const { return {}; }
size_t RealtimeAudio::target_frames() const { return 0; }
#endif

} // namespace oa::media

// Behavioral tests: a blocked UI or a slow new voice must not interrupt BGM;
// replaced sounds must not receive the previous playback's completion.
#include "core/media/audio.h"
#include "core/media/audio_realtime.h"
#include "core/media/audio_ring.h"
#include "ogg_fixture.h"
#include "SDL3/SDL.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace oa::media;
using Clock = std::chrono::steady_clock;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void ring_stress() {
    AudioRing<uint64_t> ring(1024);
    std::atomic<bool> finished{false};
    std::atomic<int> corrupt{0};
    std::thread producer([&] {
        std::array<uint64_t, 61> data;
        uint64_t next = 1;
        for (int i = 0; next < 500000; ++i) {
            for (size_t n = 0; n < data.size(); ++n) data[n] = next + n;
            next += ring.push(data.data(), data.size());
            if (i % 137 == 0) ring.clear();
            if (!ring.writable()) std::this_thread::yield();
        }
        finished.store(true);
    });
    std::thread consumer([&] {
        std::array<uint64_t, 47> data;
        uint64_t previous = 0;
        do {
            const size_t n = ring.pop(data.data(), data.size());
            for (size_t i = 0; i < n; ++i) {
                if (data[i] <= previous) ++corrupt;
                previous = data[i];
            }
            if (!n) std::this_thread::yield();
        } while (!finished.load() || ring.available());
    });
    producer.join(); consumer.join();
    check(corrupt.load() == 0, "concurrent wrap/flush never reuses in-flight PCM storage");
}
std::vector<float> decode_reference(const std::vector<uint8_t>& bytes, int channels) {
    auto source = VorbisSource::open(bytes);
    std::vector<float> result, scratch(size_t(channels) * 1024);
    for (long n; (n = source->read(scratch.data(), 1024)) > 0;)
        for (long f = 0; f < n; ++f) for (int c = 0; c < channels; ++c)
            result.push_back(scratch[size_t(c) * 1024 + size_t(f)]);
    return result;
}
void decoder_reference() {
    for (int channels : {1, 2}) for (long rate : {32000, 48000}) {
        auto a = oafix::tone_ogg(0.193, channels, rate, 317, 0.4f);
        auto b = oafix::tone_ogg(0.137, channels, rate, 631, 0.25f);
        const auto first = decode_reference(a, channels), loop = decode_reference(b, channels);
        const size_t na = first.size() / size_t(channels), nb = loop.size() / size_t(channels);
        MediaPlayers::Player player;
        player.src = VorbisSource::open(a);
        player.src_ch = player.avail_ch = channels;
        player.rate = rate; player.loop_play = true; player.loop_file = "b";
        int loop_reads = 0;
        const RealtimeAudio::Loader loader = [&](const std::string&) -> std::optional<std::vector<uint8_t>> {
            ++loop_reads; return b;
        };
        std::array<float, 512> output;
        double position = 0;
        float error = 0;
        for (int block = 0; block < 100; ++block) {
            check(decode_audio_block(player, loader, output.data(), 256) == 256, "loop decoder produces a complete block");
            for (size_t f = 0; f < 256; ++f) {
                const double p = position + double(f) * double(rate) / 44100.0;
                const size_t k = size_t(std::floor(p)); const float t = float(p - double(k));
                for (int c = 0; c < 2; ++c) {
                    const int sc = channels == 1 ? 0 : c;
                    auto sample = [&](size_t n) {
                        return n < na ? first[n * size_t(channels) + size_t(sc)] :
                            loop[((n - na) % nb) * size_t(channels) + size_t(sc)];
                    };
                    const float expected = sample(k) + (sample(k + 1) - sample(k)) * t;
                    error = std::max(error, std::fabs(expected - output[f * 2 + size_t(c)]));
                }
            }
            position += 256.0 * double(rate) / 44100.0;
        }
        check(error < 0.00001f && loop_reads == 1, "background resampling and A/B loops match independently decoded PCM");
        std::printf("decoder channels=%d rate=%ld max_error=%g\n", channels, rate, double(error));
    }
}
void pause_clock() {
    // Check actual mixed samples: a long suspension must not finish a fade,
    // including when a newer runtime snapshot arrives during the suspension.
    const auto out = std::filesystem::temp_directory_path() /
        ("oa_realtime_pause_" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(out);
    const auto prefix = (out / "samples").string();
    SDL_setenv("OA_AUDIO_REC", prefix.c_str(), 1);
    const auto tone = oafix::tone_ogg(2.0, 2, 44100, 317, 0.3f);
    const auto reference = decode_reference(tone, 2);
    MediaPlayers mp;
    AudioEngine engine;
    mp.set_loader([&](const std::string&) -> std::optional<std::vector<uint8_t>> { return tone; });
    check(mp.init() && mp.enable_realtime(), "pause clock device starts");
    BgmConfig config; config.fade_in_ms = 1000;
    engine.play_bgm("fade", config); mp.update(0, engine, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mp.set_realtime_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const size_t first = mp.realtime_stats().mixed_frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    mp.set_realtime_paused(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    mp.set_realtime_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const size_t second = mp.realtime_stats().mixed_frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.update(180); mp.update(180, engine, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mp.set_realtime_paused(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    mp.set_realtime_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const size_t total = mp.realtime_stats().mixed_frames;
    mp.release();
    std::ifstream input(prefix + ".mix.wav", std::ios::binary);
    std::vector<float> samples(total * 2);
    input.seekg(56); // signal capture's fixed IEEE-float WAV header
    input.read(reinterpret_cast<char*>(samples.data()), samples.size() * sizeof(float));
    check(bool(input), "pause regression reads captured mixer PCM");
    check(first >= 1024 && second >= first + 1024 && total >= second + 1024 &&
          samples.size() <= reference.size(), "resume produces audio without another frame update");
    if (input && first >= 1024 && second >= first + 1024 && total >= second + 1024 &&
        samples.size() <= reference.size()) {
        auto gain = [&](size_t begin) {
            double actual = 0, expected = 0;
            for (size_t i = begin * 2; i < (begin + 1024) * 2; ++i) {
                actual += std::fabs(samples[i]); expected += std::fabs(reference[i]);
            }
            return actual / expected;
        };
        const double before = gain(first - 1024), resumed = gain(first), snapshot = gain(second);
        check(resumed > 0.04 && resumed < 0.35 && std::fabs(resumed - before) < 0.13,
              "400ms pause does not advance the captured fade envelope");
        check(snapshot > 0.10 && snapshot < 0.35,
              "snapshot during pause freezes only time after the new anchor");
        std::printf("pause fade gains: before=%g resumed=%g snapshot=%g\n", before, resumed, snapshot);
    }
}
void realtime_behavior() {
    const auto main_thread = std::this_thread::get_id();
    const auto tone = oafix::tone_ogg(0.193, 2, 44100, 317, 0.3f);
    MediaPlayers mp;
    AudioEngine engine;
    std::atomic<bool> slow_started{false}, wrong_thread{false};
    std::atomic<int> slow_reads{0};
    mp.set_loader([&](const std::string& name) -> std::optional<std::vector<uint8_t>> {
        if (std::this_thread::get_id() == main_thread) wrong_thread.store(true);
        if (name == "slow") {
            ++slow_reads; slow_started.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        }
        return tone;
    });
    mp.set_path_resolver([&](const std::string& name) {
        if (std::this_thread::get_id() != main_thread) wrong_thread.store(true);
        return name;
    });
    check(mp.init() && mp.enable_realtime(), "native realtime producer starts on a dummy device");
    BgmConfig bgm;
    engine.play_bgm("bgm", bgm);
    int finishes = 0;
    MediaPlayers::FinishFn finish = [&](SoundCategory cat, const std::string& id) {
        check(std::this_thread::get_id() == main_thread, "completion is delivered on the runtime thread");
        ++finishes;
        if (cat == SoundCategory::Bgm) engine.stop_bgm(0); else engine.stop_se(id, 0);
    };
    auto tick = [&](uint64_t delta = 16) {
        engine.update(delta); mp.update(delta, engine, finish);
    };
    for (int i = 0; i < 20; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    const uint64_t underruns = mp.device_underruns(0);
    const auto before = mp.realtime_stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // no runtime/frame updates
    const auto after = mp.realtime_stats();
    check(after.mixed_frames - before.mixed_frames >= 44100 * 400 / 1000,
          "BGM keeps producing while the main thread is stalled for 500ms");
    check(mp.device_underruns(0) == underruns, "main-thread stall creates no device short reads");
    SeConfig voice;
    engine.play_voice("voice", "slow", voice);
    auto begin = Clock::now(); tick();
    const auto submit_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
    for (int i = 0; i < 35; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    const auto loaded = mp.realtime_stats();
    check(slow_started.load() && slow_reads.load() == 1 && loaded.max_load_us >= 300000,
          "test actually performs one slow resource load");
    check(submit_us < 100000 && mp.device_underruns(0) == underruns,
          "slow voice loading neither blocks frame submission nor interrupts BGM");
    check(loaded.source_short_frames == 0, "decoder source rings do not starve during the blocked UI/load arms");
    check(!wrong_thread.load(), "only path resolution and completion access the runtime thread");
    mp.set_realtime_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto pause_frames = mp.realtime_stats().mixed_frames;
    const auto pause_waits = mp.realtime_stats().mixer_waits;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("paused_100ms_waits=%llu\n", (unsigned long long)(mp.realtime_stats().mixer_waits - pause_waits));
    check(mp.realtime_stats().mixer_waits == pause_waits, "explicit pause waits for events without polling");
    check(mp.realtime_stats().mixed_frames == pause_frames, "background pause stops the independent producer");
    mp.set_realtime_paused(false);
    for (int i = 0; i < 8; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    check(mp.realtime_stats().mixed_frames > pause_frames, "foreground resume restarts the producer");
    // Leave an old EOF in the mailbox, replay the exact same file/id without
    // advancing the engine clock, and verify that the old event is rejected.
    engine.play_se("replay", "short", voice);
    mp.update(0, engine, finish);
    const auto old_id = engine.state().se_channels.at("replay").playback_id;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    engine.play_se("replay", "short", voice);
    check(old_id != engine.state().se_channels.at("replay").playback_id, "same-clock replays have distinct identities");
    const int finished_before = finishes;
    mp.update(0, engine, finish);
    check(engine.is_se_playing("replay"), "stale EOF does not stop a replacement sound");
    for (int i = 0; i < 28; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    check(finishes == finished_before + 1, "replacement sound completes exactly once");
    engine.stop_bgm(80);
    for (int i = 0; i < 8; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    check(!engine.is_bgm_playing(), "fade-out stop retains its main-thread state semantics");
    engine.stop_all_sounds(); mp.stop_all(); mp.update(0, engine, finish);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto stopped = mp.realtime_stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(stopped.active_streams == 0 && mp.realtime_stats().mixed_frames == stopped.mixed_frames,
          "reset removes streams and stops producing old audio");
    std::printf("idle_60ms_waits=%llu playing_500ms_waits=%llu\n",
        (unsigned long long)(mp.realtime_stats().mixer_waits - stopped.mixer_waits),
        (unsigned long long)(after.mixer_waits - before.mixer_waits));
    check(mp.realtime_stats().mixer_waits == stopped.mixer_waits, "idle mixer waits for events without polling");
    // Start from the indefinitely idle wait and make no further submissions.
    // Loader/decode readiness must wake the mixer on its own.
    engine.play_bgm("bgm", bgm); mp.update(0, engine, finish);
    const auto restart = Clock::now();
    while (mp.realtime_stats().mixed_frames == stopped.mixed_frames &&
           Clock::now() - restart < std::chrono::milliseconds(200))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(mp.realtime_stats().mixed_frames > stopped.mixed_frames, "one submission wakes idle loader, decoder and mixer");
    std::printf("realtime: stall_frames=%llu source_short=%llu slow_load_us=%llu submit_us=%lld callbacks=%llu underruns=%llu\n",
        (unsigned long long)(after.mixed_frames - before.mixed_frames),
        (unsigned long long)loaded.source_short_frames, (unsigned long long)loaded.max_load_us,
        (long long)submit_us, (unsigned long long)mp.device_callbacks(),
        (unsigned long long)mp.device_underruns(0));
    mp.release();
    // Negative control proves that this test detects the old frame-fed design.
    MediaPlayers legacy; AudioEngine old;
    legacy.set_loader([&](const std::string&) -> std::optional<std::vector<uint8_t>> { return tone; });
    check(legacy.init(), "negative-control device starts");
    old.play_bgm("bgm", bgm);
    for (int i = 0; i < 15; ++i) {
        old.update(16); legacy.update(16, old, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const auto prior = legacy.device_underruns(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(legacy.device_underruns(0) > prior, "negative control exposes frame-fed starvation");
    std::printf("legacy: stall_short_reads=%llu\n", (unsigned long long)(legacy.device_underruns(0) - prior));
    legacy.release();
}
}
int main() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    ring_stress(); decoder_reference(); pause_clock(); realtime_behavior();
    SDL_Quit();
    std::printf("audio_realtime: failures=%d\n", failures);
    return failures ? 1 : 0;
}

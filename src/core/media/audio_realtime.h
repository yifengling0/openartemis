#pragma once

#include "core/media/audio.h"

namespace oa::media {

// Owns no runtime/Lua objects. Only resolved paths and copied channel state
// cross this boundary; completion delivery stays on the submitting thread.
class RealtimeAudio {
public:
    using Loader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;
    RealtimeAudio(AudioSink& sink, Loader loader);
    ~RealtimeAudio();
    void submit(AudioEngine::State state);
    void stop_all();
    void set_paused(bool paused);
    void poll(AudioEngine& engine, const MediaPlayers::FinishFn& finish);
    MediaPlayers::RealtimeStats stats() const;
    size_t target_frames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Synchronous source-domain decode/resample, shared with the deterministic
// mixer. Called exclusively by a decode job, never by the device callback.
size_t decode_audio_block(MediaPlayers::Player& player,
                         const RealtimeAudio::Loader& loader,
                         float* output, size_t frames);

} // namespace oa::media

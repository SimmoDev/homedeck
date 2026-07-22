#include "platform/host/audio_output.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace homedeck {

namespace {

// SDL_QueueAudio has no built-in volume control - scale a local copy
// rather than SDL_MixAudioFormat's more involved dest-buffer API, or
// mutating the caller's own buffer (unlike FirmwareAudioOutput's
// in-place esp_codec_dev_write(), which owns that tradeoff on
// firmware for a real reason - here there isn't one).
std::vector<int16_t> ScaleVolume(const int16_t* samples, size_t sample_count, int volume_percent) {
    std::vector<int16_t> scaled(sample_count);
    double gain = std::clamp(volume_percent, 0, 100) / 100.0;
    for (size_t i = 0; i < sample_count; ++i) {
        scaled[i] = static_cast<int16_t>(samples[i] * gain);
    }
    return scaled;
}

}  // namespace

bool HostAudioOutput::Play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    // Reference-counted per subsystem - safe to call alongside LVGL's
    // own SDL_INIT_VIDEO (via lv_sdl_window_create(), src/ui/ui_task.cpp),
    // additive rather than re-initializing anything already active.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec = {};
    spec.freq = static_cast<int>(sample_rate);
    spec.format = AUDIO_S16SYS;
    spec.channels = 1;
    spec.samples = 4096;

    SDL_AudioDeviceID device = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device == 0) {
        // No audio device available (e.g. a headless sandbox) - not a
        // bug in this class, just nothing to play through. Matches
        // this project's "degrade gracefully" offline-behavior
        // philosophy applied to a missing local device instead of a
        // missing network.
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    std::vector<int16_t> scaled = ScaleVolume(samples, sample_count, volume_percent_);
    SDL_QueueAudio(device, scaled.data(), static_cast<Uint32>(scaled.size() * sizeof(int16_t)));
    SDL_PauseAudioDevice(device, 0);

    // SDL_QueueAudio() returns immediately (queued, not played) - sleep
    // for the clip's real duration so Play() blocks until playback
    // finishes, matching esp_codec_dev_write()'s own blocking contract
    // on firmware (see platform/audio_output.h).
    auto duration_ms = static_cast<int64_t>(sample_count) * 1000 / sample_rate;
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    SDL_CloseAudioDevice(device);
    return true;
}

void HostAudioOutput::SetVolume(int percent) {
    volume_percent_ = std::clamp(percent, 0, 100);
}

}  // namespace homedeck

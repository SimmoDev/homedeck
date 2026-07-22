#pragma once

#include "platform/audio_output.h"

namespace homedeck {

// Backs AudioOutput with SDL2 audio (SDL_QueueAudio) - not built by
// homedeck_platform_host (see src/CMakeLists.txt's own comment at the
// call site): that library is shared with tests/, which never fetches
// LVGL/SDL2. This is compiled into homedeck_ui instead, the same
// if(TARGET lvgl)-gated library ui_task.cpp (LVGL's own SDL display
// driver) already lives in - the first src/platform/host/ file not
// built unconditionally, deliberately, not an oversight.
class HostAudioOutput : public AudioOutput {
public:
    bool Play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) override;
    void SetVolume(int percent) override;

private:
    int volume_percent_ = 100;
};

}  // namespace homedeck

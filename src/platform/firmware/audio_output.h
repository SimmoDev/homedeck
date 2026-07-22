#pragma once

#include "platform/audio_output.h"

#include "esp_codec_dev.h"

namespace homedeck {

// Backs AudioOutput with the ES8388 speaker codec via the vendored
// M5Stack Tab5 BSP's bsp_audio_codec_speaker_init() (I2S + I2C + the
// BSP_FEATURE_SPEAKER IO-expander enable, all handled internally - see
// docs/architecture/hardware.md#audio) and Espressif's esp_codec_dev
// library for playback.
class FirmwareAudioOutput : public AudioOutput {
public:
    FirmwareAudioOutput();

    bool Play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) override;
    void SetVolume(int percent) override;

private:
    esp_codec_dev_handle_t codec_;
};

}  // namespace homedeck

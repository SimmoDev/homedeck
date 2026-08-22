#include "platform/firmware/audio_output.h"

#include "bsp/m5stack_tab5.h"
#include "esp_log.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "audio_output";

}  // namespace

FirmwareAudioOutput::FirmwareAudioOutput() : codec_(bsp_audio_codec_speaker_init()) {
    if (codec_ == nullptr) {
        ESP_LOGE(kTag, "bsp_audio_codec_speaker_init() failed");
    }
}

bool FirmwareAudioOutput::Play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    if (codec_ == nullptr) {
        return false;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = 1;
    fs.bits_per_sample = 16;

    int open_result = esp_codec_dev_open(codec_, &fs);
    if (open_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "esp_codec_dev_open failed: %d", open_result);
        return false;
    }
    // esp_codec_dev_write() takes a non-const void* - the codec/volume
    // layer may adjust sample levels in place (see esp_codec_dev.h's
    // own "when software volume is enabled, it changes input data
    // directly without copy" note), so it genuinely needs write access
    // to the buffer, not just a cast-away-const formality.
    int write_result = esp_codec_dev_write(codec_, const_cast<int16_t*>(samples),
                                            static_cast<int>(sample_count * sizeof(int16_t)));
    int close_result = esp_codec_dev_close(codec_);
    if (close_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "esp_codec_dev_close failed: %d", close_result);
    }

    if (write_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "esp_codec_dev_write failed: %d", write_result);
        return false;
    }
    return true;
}

void FirmwareAudioOutput::SetVolume(int percent) {
    if (codec_ == nullptr) {
        return;
    }
    int result = esp_codec_dev_set_out_vol(codec_, percent);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "esp_codec_dev_set_out_vol failed: %d", result);
    }
}

}  // namespace homedeck

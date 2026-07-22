#pragma once

#include <cstddef>
#include <cstdint>

namespace homedeck {

// Outbound audio - see docs/architecture/hardware.md#audio. Small and
// virtual for the same reason as BatteryReader/NetworkStatus/HttpClient:
// a simple operation where mockability matters directly, not a
// performance-sensitive primitive.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Mono 16-bit PCM. Blocking - paces itself in real time against
    // sample_rate (esp_codec_dev_write()'s own semantics on firmware;
    // matched on the simulator so callers see identical timing on both
    // targets). Never call from the LVGL/UI-owning thread (the one
    // running lv_timer_handler()) on either target - it stalls
    // rendering and input for the clip's whole duration. A single
    // short clip is fine to call directly from a non-UI thread
    // (app_main's own thread, before the dashboard's event loop takes
    // over, is fine); anything longer, repeated, or triggered from a
    // UI callback should run on its own Task (platform/task.h), the
    // same pattern OpenMeteoWeatherProvider's poll Task already
    // establishes for other blocking hardware work.
    virtual bool Play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) = 0;

    virtual void SetVolume(int percent) = 0;  // 0-100
};

}  // namespace homedeck

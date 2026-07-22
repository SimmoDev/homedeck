#include "crash_diagnostics.h"

#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "crash_diag";

// ESP-IDF doesn't provide a string form of esp_reset_reason_t itself.
const char* ResetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power-on";
        case ESP_RST_EXT:
            return "external-pin";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt-watchdog";
        case ESP_RST_TASK_WDT:
            return "task-watchdog";
        case ESP_RST_WDT:
            return "other-watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep-sleep-wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse-error";
        case ESP_RST_PWR_GLITCH:
            return "power-glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu-lockup";
        case ESP_RST_UNKNOWN:
        default:
            return "unknown";
    }
}

}  // namespace

void LogCrashDiagnostics(Storage& storage) {
    const char* reason = ResetReasonToString(esp_reset_reason());
    ESP_LOGI(kTag, "Last reset reason: %s", reason);

    storage.SetSetting("core", "reset_reason", 1, reason);

    bool has_core_dump = esp_core_dump_image_check() == ESP_OK;
    storage.SetSetting("core", "has_core_dump", 1, has_core_dump ? "true" : "false");

    // Raw task name/program counter only - not symbolicated on-device
    // (see ADR-0013). A developer decodes the actual downloaded core
    // dump off-device against the matching build. Never erased here or
    // by the Web UI's own download endpoint (`core/diagnostics_routes.cpp`) -
    // erasure isn't implemented anywhere yet.
    if (has_core_dump) {
        esp_core_dump_summary_t summary;
        if (esp_core_dump_get_summary(&summary) == ESP_OK) {
            ESP_LOGW(kTag, "Core dump present: task='%s' pc=0x%08lx", summary.exc_task,
                     static_cast<unsigned long>(summary.exc_pc));
        } else {
            ESP_LOGW(kTag, "Core dump present but summary unavailable");
        }
    }
}

}  // namespace homedeck

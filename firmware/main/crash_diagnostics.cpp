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

    if (!storage.SetSetting("core", "reset_reason", 1, reason)) {
        ESP_LOGW(kTag, "Failed to persist reset_reason");
    }

    bool has_core_dump = esp_core_dump_image_check() == ESP_OK;
    if (!storage.SetSetting("core", "has_core_dump", 1, has_core_dump ? "true" : "false")) {
        ESP_LOGW(kTag, "Failed to persist has_core_dump");
    }

    // Raw registers only - not symbolicated on-device (see ADR-0013). A
    // developer decodes the actual downloaded core dump off-device
    // against the matching build for a real backtrace. Never erased here
    // or by the Web UI's own download endpoint
    // (`core/diagnostics_routes.cpp`) - erasure isn't implemented
    // anywhere yet.
    //
    // ex_info (RISC-V-specific: mcause/mtval/ra/sp/a0-a7 - see
    // esp_core_dump_summary_port.h) carries the trap cause/value/
    // return-address/stack-pointer/argument registers, not just
    // task/PC. mcause identifies the exact RISC-V trap cause (1 =
    // instruction access fault, matching the "Guru Meditation Error"
    // seen on the serial console); mtval is typically the actual
    // faulting address for an access fault, which may or may not match
    // exc_pc - a mismatch would itself be a real clue. See
    // docs/architecture/hardware.md#wi-fi-bring-up for the specific
    // crash this currently helps diagnose.
    if (has_core_dump) {
        esp_core_dump_summary_t summary;
        if (esp_core_dump_get_summary(&summary) == ESP_OK) {
            ESP_LOGW(kTag, "Core dump present: task='%s' pc=0x%08lx", summary.exc_task,
                     static_cast<unsigned long>(summary.exc_pc));
            ESP_LOGW(kTag, "  mcause=0x%08lx mtval=0x%08lx ra=0x%08lx sp=0x%08lx",
                     static_cast<unsigned long>(summary.ex_info.mcause),
                     static_cast<unsigned long>(summary.ex_info.mtval),
                     static_cast<unsigned long>(summary.ex_info.ra),
                     static_cast<unsigned long>(summary.ex_info.sp));
            for (int i = 0; i < 8; ++i) {
                ESP_LOGW(kTag, "  a%d=0x%08lx", i, static_cast<unsigned long>(summary.ex_info.exc_a[i]));
            }
        } else {
            ESP_LOGW(kTag, "Core dump present but summary unavailable");
        }
    }
}

}  // namespace homedeck

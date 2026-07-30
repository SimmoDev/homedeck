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

    // Raw registers only - not symbolicated on-device (see ADR-0013). A
    // developer decodes the actual downloaded core dump off-device
    // against the matching build for a real backtrace. Never erased here
    // or by the Web UI's own download endpoint
    // (`core/diagnostics_routes.cpp`) - erasure isn't implemented
    // anywhere yet.
    //
    // ex_info (RISC-V-specific: mcause/mtval/ra/sp - see
    // esp_core_dump_summary_port.h) used to be discarded entirely, only
    // exc_task/exc_pc were logged - a real diagnostic gap for a crash
    // this project has been trying to root-cause across multiple
    // sessions (see docs/architecture/hardware.md#wi-fi-bring-up).
    // mcause identifies the exact RISC-V trap cause (1 = instruction
    // access fault, matching the "Guru Meditation Error" already seen on
    // the serial console); mtval is typically the actual faulting
    // address for an access fault, which may or may not match exc_pc -
    // a mismatch would itself be a real clue.
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

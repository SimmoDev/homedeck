# ADR-0028: Time Synchronization

## Status

Accepted

## Context

[hardware.md](../architecture/hardware.md#rtc) flagged the RX8130CE RTC as
"never been set" from the moment it was first brought up in M1: the chip
supplies a supercapacitor-backed clock, but nothing in the codebase has ever
written a correct value into it, so it reads a meaningless factory/power-on
date on the reference unit. That same document named the fix as needing
"either SNTP over Wi-Fi or a manual set-time affordance" and called both "M2
scope" — but no [roadmap.md](../roadmap.md) item, checked or unchecked, was
ever created for either, so the gap fell out of milestone tracking entirely
rather than being delivered or explicitly deferred: the always-on status bar
clock and every
structured log timestamp (`Logger`,
[ADR-0019](ADR-0019-structured-logging.md)) are both wrong on real
hardware, indefinitely, with no path to fixing it.

## Decision

**SNTP over Wi-Fi**, not a manual set-time Web UI/Touch UI field. Once the
device has Wi-Fi (this project's whole reason a reliable wall clock matters
beyond a single boot), a public NTP pool gives an always-correct time with no
per-device setup step - a manual field would require a person to actually set
it, on every device, and wouldn't self-correct if left wrong or if the RTC's
backup eventually drifts. A manual affordance remains a plausible future
addition for permanently-offline installations, but isn't built now - no
resolved gap prompted it, unlike SNTP resolving a real, already-flagged one.

**Mechanism:** ESP-IDF's `esp_netif_sntp` component (`esp_netif_sntp_init()`
with `ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org")`, `firmware/main/homedeck.cpp`),
started once Wi-Fi first connects. No per-user NTP-server configuration
exists - the same "no premature abstraction ahead of a real need" reasoning
[dashboard.md](../architecture/dashboard.md#weather-source)'s direct
Open-Meteo default already follows. `wait_for_sync` stays at its own default
(`false`): `app_main()` never blocks waiting for a sync that might not come
(Wi-Fi connected but no internet route is a real, unremarkable case for a
local-first device) - the sync callback (`OnSntpTimeSync`) fires whenever a
sync eventually succeeds, however long that takes.

**Every sync, not just the first, writes back to the physical RTC** via a
new `Rx8130TimeSource::SetTime()` (`src/platform/firmware/time_source.h`/
`.cpp`, wrapping `espp::Rx8130ce::set_time()`) - LwIP's SNTP client
periodically resyncs on its own (`CONFIG_LWIP_SNTP_UPDATE_DELAY`, 1 hour by
default, left at that default), and persisting each correction to the RTC's
supercap-backed storage (not just the ESP32's own volatile system clock)
means the correction survives a reboot even after a short-uptime session,
consistent with why a hardware RTC was chosen over relying on the system
clock alone ([ADR-0016](ADR-0016-battery-rtc-library.md)).

**No timezone handling is added.** `Rx8130TimeSource::Now()` already
interpreted the RTC's raw fields as local wall time directly (via
`std::mktime`, no TZ conversion) — a pre-existing, already-documented gap,
not something introduced or worsened here. `SetTime()` writes the SNTP
result's UTC fields via `gmtime_r` with the identical "no TZ math" treatment,
so writing and reading stay consistent with each other; the net effect is
that the clock is now *correct in UTC* rather than *meaningless*, a strict
improvement, but still not corrected to the user's actual local timezone.
Adding real timezone support (a Web UI setting, offset/DST math) is a
separate, genuinely new feature, not a natural extension of fixing RTC
calibration - noted in [roadmap.md](../roadmap.md)'s M2 RTC item as a
future item not yet placed against a specific milestone, not solved
here.

**esp_sntp_config_t's `sync_cb` is a plain C function pointer with no
`user_data` slot**, so it can't close over `Rx8130TimeSource`/`Logger`
directly. Bridged via two file-scope pointers in `homedeck.cpp`
(`g_time_source_for_sntp`/`g_logger_for_sntp`), set once during the
single-threaded boot sequence before Wi-Fi (and therefore any possible
sync) can occur - the same category of exception `wifi_setup.cpp`'s own
`g_state` already takes for an identical reason
([ADR-0026](ADR-0026-wifi-provisioning-mechanism.md)'s Consequences), not a
new pattern being introduced.

**Simulator:** no change needed. The host OS already keeps correct time via
its own NTP/clock sync; `HostTimeSource` reads `std::chrono::system_clock`
directly. SNTP, like crash/reboot diagnostics
([diagnostics.md](../architecture/diagnostics.md#status)) and mDNS
self-advertisement ([networking.md](../architecture/networking.md#status)),
is a firmware-only mechanism with nothing to simulate on the host side.

## Consequences

- [hardware.md](../architecture/hardware.md#rtc)'s RTC section's "never been set" framing is now stale and
  needs updating to reflect that it's corrected once Wi-Fi (and internet
  reachability) is available - a device that never gets internet access
  still shows whatever it held before this feature existed, degrading
  gracefully rather than failing loudly, consistent with this project's
  offline-behavior philosophy.
- Structured log timestamps ([ADR-0019](ADR-0019-structured-logging.md))
  are only as accurate as the RTC is at the moment each entry is logged -
  entries from before the first successful sync (e.g. very early boot, or
  a device with no internet route yet) keep whatever the RTC previously
  held.
- Timezone support remains a real, separate gap - tracked as its own future
  item, not solved by this ADR.
- No new managed component dependency - `esp_netif_sntp.h` is part of the
  already-required `esp_netif` component.

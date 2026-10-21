# ADR-0032: Recovery Access Point Requires On-Device Approval

## Status

Accepted. Extends [ADR-0026](ADR-0026-wifi-provisioning-mechanism.md): the
open SoftAP and unauthenticated form it accepts are scoped there to the
first-run setup window. This ADR covers the recovery access point, which
runs on an already-provisioned device.

## Context

A provisioned device that cannot reach its stored network for about two
minutes (40 failed reconnects) brings up a second open SoftAP and the same
setup form on port 8080, alongside its continuing reconnect attempts. It
exists so a device whose network is gone for good (moved house, router
replaced) is not stranded: the Web UI is unreachable without Wi-Fi, and the
Touch UI otherwise has no way back into Wi-Fi setup.

ADR-0026 accepts an open AP with no authentication because the first-run
window is brief and physically local. The recovery window is neither: it
opens whenever the home network is down, including a router reboot that
outlasts the threshold, or an attacker deauthenticating the device for two
minutes. If the form applied what it received, anyone in radio range could
overwrite the stored Wi-Fi credentials with no admin password and move the
device onto a network they control.

## Decision

On the recovery access point a form submission is a request, not a change.
`wifi_setup.cpp` validates it, keeps it as the one pending request
(`PendingWifiCredentials`, `src/core/pending_wifi_credentials.h`) and tells
the Touch UI, which navigates to `WifiSetupScreen` and shows the requested
network name with Allow and Deny buttons. Only Allow applies the
credentials. The tap is the proof that someone is at the device.

- Each request has an id and Allow carries the id that was shown, so a
  submission that arrives between the prompt and the tap cannot be applied
  by that tap; it gets its own prompt.
- A newer request replaces the older one, so at most one is pending.
- The request is discarded on Deny and when the device connects.
- The first-run form is unchanged: with no stored credentials there is
  nothing to overwrite and the device has nothing else to offer. Credentials
  typed on the Touch UI's own keyboard apply directly in both modes.

**Rejected - leaving the recovery form as the first-run form is.** It makes
the stored credentials writable by anyone in range whenever the network is
down.

**Rejected - removing the recovery access point.** A device whose network is
gone would need a factory reset and reflash to be reconfigured.

## Consequences

- The recovery access point remains open and unencrypted. A visitor can
  still connect to it, load the form and submit a request; they cannot
  change anything. They can repeatedly prompt the person at the device, who
  can Deny.
- Recovering from a browser now needs someone at the device to tap Allow.
  The Touch UI keyboard path still works without that step.
- [security.md](../architecture/security.md) and
  [networking.md](../architecture/networking.md) describe the behaviour.

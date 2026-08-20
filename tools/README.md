# tools/

Supporting developer tooling for HomeDeck. Each script is a thin wrapper
around commands documented in full (including the "why" behind each
flag) in [DEVELOPMENT.md](../DEVELOPMENT.md)'s ESP-IDF setup section -
read that first if a script here does something unexpected.

- `flash.sh [serial-port]` - builds and flashes firmware onto Tab5
  hardware (default port `/dev/ttyACM0`).
- `monitor.sh [serial-port]` - opens an interactive serial monitor.
- `factory-reset.sh [serial-port]` - erases the device's entire flash
  (settings, secrets, OTA state, the firmware image itself); confirms
  before proceeding since it's irreversible. Run `flash.sh` afterward to
  make the device bootable again.
- `set-password.sh [host]` - sets the Web UI admin password on a device
  that doesn't have one yet (default host `homedeck.local` - pass an IP
  if mDNS resolution doesn't work on your machine).

All four target the K145 reference unit.

## Commit hooks

Two git hook stages. Only the secret scan blocks a commit; every other
check is warn-only - see `githooks/pre-commit`'s own header comment for
why that split exists. Checking for the following defect classes:

- `githooks/pre-commit` runs against staged doc/code files: doc
  narration/banned wording/stale ADR cross-references, broken relative
  Markdown links and anchors, and a staged `tests/*_test.cpp` file
  missing from [tests/README.md](../tests/README.md)'s own test inventory
  (`githooks/check-docs.sh`); [hardware.md](../docs/architecture/hardware.md)'s
  documented electrical/physical-facts-only scope, when that file is
  staged (`githooks/check-hardware-md-scope.sh`); unchecked ESP-IDF/mDNS/httpd
  return values in `firmware/main/` and `src/platform/firmware/`
  (`githooks/check-esp-idf-returns.sh`); a `curl_easy_perform()`
  call with no `CURLOPT_TIMEOUT`/`CURLOPT_CONNECTTIMEOUT` set anywhere
  in the same file, in any staged `.cpp` file
  (`githooks/check-curl-timeouts.sh`); the same rule for
  `esp_http_client_perform()` (no `config.timeout_ms` set anywhere in
  the file), scoped to the same `firmware/main/`/`src/platform/firmware/`
  files as the ESP-IDF-return check above - `esp_websocket_client_start()`
  is deliberately not covered by this one, since its own config struct's
  `network_timeout_ms` already defaults to a bounded 10s, unlike
  libcurl's own multi-minute default
  (`githooks/check-esp-http-timeouts.sh`); and, against every staged file
  regardless of extension, private-key blocks, cloud-provider/API-token
  credential shapes, and a staged `.env` file - the one check that
  actually blocks the commit
  (`githooks/check-secrets.sh`).
- `githooks/commit-msg` runs against the commit message itself, once
  written - the same narration patterns check-docs.sh checks in files
  (`githooks/lib-narration-patterns.sh`, shared by both), since a commit
  message can narrate a review/testing process just as easily as a doc
  can. This doesn't ban process detail from commit messages -
  [CLAUDE.md](../CLAUDE.md)'s own Documentation section says git history
  is exactly where "how and why" belongs - only the same verification-log
  *phrasing* (pass/round tallies, "found N issues, fixed M") the doc
  checks already flag.

Activate both once per clone:

```sh
git config core.hooksPath tools/githooks
```

The scripts are tracked in the repo like any other file; only the
`core.hooksPath` setting itself is local to your clone and needs
re-running after a fresh checkout. `git commit --no-verify` skips it, as
with any hook.

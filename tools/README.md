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

## Kodi on-hardware checks

`kodi-hardware-test/` supports the Kodi part of a Tab5 verification pass
against a Kodi running on the development machine. Needs Python 3, and
`pip install websocket-client` for `block_kodi.py`. The admin password goes
in `HOMEDECK_PASSWORD`.

- `device_check.py discovery` - the device is connected to Kodi through
  an address found by mDNS discovery.
- `device_check.py restart` - stops the local Kodi, expects the device to
  report the link down, starts Kodi again and expects the device to
  reconnect.
- `device_check.py monitor [seconds]` - watches the busy flag while you
  tap through the Touch UI; passes when it is raised, cleared and the link
  never dropped.
- `slow_source.py` - an HTTP server on `127.0.0.1:8099` that answers every
  request after 50 s, longer than the 30 s listing timeout.
- `block_kodi.py` - sends Kodi a listing of that server, which blocks every
  other JSON-RPC call on all connections for 50 s.

To exercise a timed-out listing:

1. Run `slow_source.py`. Add a video source named `SlowTest` with path
   `http://127.0.0.1:8099/` to `~/.kodi/userdata/sources.xml`. Create
   `~/.kodi/userdata/advancedsettings.xml` with `<network>` children
   `curlclienttimeout` and `curllowspeedtime` set to `120`; Kodi aborts
   a silent HTTP source after 20 s otherwise.
2. Restart Kodi, waiting for the old process to exit (a second instance
   cannot bind ports 9090 and 8080). The restart also marks every browse
   list stale; lists are cached per connection and reload only after a
   reconnect or a timed-out result.
3. With the device on the dashboard, run `block_kodi.py`, then open Movies
   (or TV Shows, Music or Live TV) on the device. After 30 s it shows
   "Kodi took too long to list this." Once `block_kodi.py` reports Kodi
   is free, leave the screen and open it again: the list loads.
   `Files > SlowTest` is the manual case, and Back then reopen retries it.
4. Restore `sources.xml`, delete `advancedsettings.xml` and stop the
   server.

## Commit hooks

Two git hook stages. Only the secret scan blocks a commit; every other
check is warn-only - see `githooks/pre-commit`'s own header comment for
why that split exists. Checking for the following defect classes:

- `githooks/pre-commit` runs against staged doc/code files: doc
  narration/banned wording/stale ADR cross-references, broken relative
  Markdown links and anchors, filler-adverb warnings (a wider word list in Markdown outside
  `docs/decisions/`), and a staged `tests/*_test.cpp`, `webui/src/lib/*.svelte`
  or tool script missing from its directory README's inventory
  ([tests/](../tests/README.md), [webui/](../webui/README.md), this file)
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
  (`githooks/check-esp-http-timeouts.sh`); a `server.RegisterHandler(...)`
  call with no `RequireAuth(...)` wrapper within 5 lines, in any staged
  `.cpp` file, excluding the auth routes themselves and static asset
  serving - the two known-legitimate unauthenticated route registrations
  (`githooks/check-unauthenticated-routes.sh`); an `nlohmann::json`
  `.value(key, default)` call whose default isn't itself a json value
  (a type conversion to the default's own type, not an identity one), or
  an `.at(key).get<T>()`
  call with no `is_number()`/`is_string()`/`is_boolean()`/`is_object()`/
  `is_array()` check for that same key anywhere else in the file - both
  throw `json::type_error` on a type-mismatched field, which is
  `std::abort()` on firmware since exceptions are compiled out there
  (`githooks/check-unchecked-json-field-access.sh`); a strict `.dump()`
  (default error handler) in a file that handles raw mDNS bytes, which
  throws on invalid UTF-8 - `std::abort()` on firmware
  (`githooks/check-json-dump-strict.sh`); a staged generated file -
  Python bytecode, object file or log - that belongs in `.gitignore`
  (`githooks/check-build-artifacts.sh`); and, against every
  staged file regardless of extension, private-key blocks,
  cloud-provider/API-token credential shapes, and a staged `.env` file -
  the one check that blocks the commit
  (`githooks/check-secrets.sh`).
- `githooks/pre-commit` also runs `shellcheck -S warning` on staged shell
  scripts (the `.sh` files and the extensionless hooks) when `shellcheck`
  is installed; `lint.yml` runs it on every script.
- `githooks/check-lvgl-version-sync.sh` takes no arguments and is not part
  of the pre-commit hook: CI (`lint.yml`) runs it, and it fails when the
  simulator's pinned LVGL release differs from the version in
  `firmware/dependencies.lock`.
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

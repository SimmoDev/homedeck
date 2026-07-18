# Third-party vendored headers

Header-only dependencies vendored directly into the repo rather than fetched
by either build system, because they need to be visible identically to
`src/CMakeLists.txt` (host/simulator/tests) *and* `firmware/main/CMakeLists.txt`
(ESP-IDF's own component build) - the two other new-dependency mechanisms
already in use elsewhere in this project (`FetchContent` in
`src/CMakeLists.txt` for host-only libraries like civetweb; `REQUIRES` in
`firmware/main/CMakeLists.txt` for ESP-IDF's own vendored components like
mbedtls) each only reach one of the two targets.

- **`nlohmann/json.hpp`** - [nlohmann::json](https://github.com/nlohmann/json)
  v3.11.3, the official single-header amalgamated release artifact (not the
  multi-file source tree), unmodified. MIT licensed. This is the JSON
  library [ADR-0002](../../docs/decisions/ADR-0002-technology-stack.md#2-json-library)
  already decided project-wide; `AdminAuthService`'s HTTP endpoints (see
  `core/admin_auth_service.cpp`) are its first real consumer.

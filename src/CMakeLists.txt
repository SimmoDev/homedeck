# The simulator's HTTP server backend (see platform/host/http_server.h) -
# civetweb v1.16's own CMakeLists.txt predates CMake's removal of
# compatibility with versions before 3.5, so CMAKE_POLICY_VERSION_MINIMUM
# must be set before fetching it, confirmed via a real scratch build
# rather than assumed. CIVETWEB_ENABLE_CXX stays off - only the plain C
# API is used (see http_server.h's own comment for why).
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
  civetweb
  GIT_REPOSITORY https://github.com/civetweb/civetweb.git
  GIT_TAG v1.16
  GIT_SHALLOW TRUE
)
set(CIVETWEB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_CXX OFF CACHE BOOL "" FORCE)
# Defaults ON in Debug builds and compiles civetweb's own object files with
# -fsanitize=..., but that flag never reaches our own targets' link line
# (this repo doesn't build with sanitizers), leaving unresolved __ubsan_*/
# __asan_* symbols at link time - confirmed via a real Debug-mode build
# failing with exactly those undefined references.
set(CIVETWEB_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(civetweb)

# AdminAuthService's password hashing/session-token randomness (see
# core/admin_auth_service.h) - mbedtls is already vendored by ESP-IDF for
# firmware (zero new firmware dependency), and is portable C, so it's
# fetched here too rather than writing a second, independent
# implementation of a security primitive for the host target. Only
# mbedcrypto (the primitive library) is linked - mbedx509/mbedtls (the
# X.509 and TLS stacks) build as part of the same CMake project but
# nothing links them, since nothing here needs TLS. Confirmed via a real
# scratch FetchContent build producing real PBKDF2/CTR_DRBG output,
# matching this project's standing "verify, don't assume" discipline for
# new dependencies.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
# mbedtls treats its own compiler warnings as errors by default -
# confirmed via the same scratch build above that this repo's GCC
# version trips some of them (mostly -Wunterminated-string-initialization
# in its TLS 1.3 code, which nothing here uses), so this stays off rather
# than gating an unrelated dependency's build on warnings in code paths
# this project doesn't exercise.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
  GIT_TAG v3.6.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(mbedtls)

# HttpClient's host backend (see platform/host/http_client.h) - system-
# linked, not FetchContent-vendored like civetweb/mbedtls above: this is
# host-only dev tooling never shipped to firmware, the same precedent
# DEVELOPMENT.md already sets for SDL2 (a required system package, not
# vendored). libcurl's own build (many optional features, SSL backend
# selection) is also meaningfully heavier to vendor from source than
# civetweb/mbedtls were.
find_package(CURL REQUIRED)

add_library(homedeck_platform_host
  platform/host/task.cpp
  platform/host/timer.cpp
  platform/host/file_backed_store.cpp
  platform/host/settings_store.cpp
  platform/host/secret_store.cpp
  platform/host/cache_store.cpp
  platform/host/http_server.cpp
  platform/host/http_client.cpp
  platform/static_assets.cpp
)
target_include_directories(homedeck_platform_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(homedeck_platform_host PUBLIC cxx_std_20)
target_link_libraries(homedeck_platform_host PUBLIC civetweb-c-library CURL::libcurl)

add_library(homedeck_core
  core/event_bus.cpp
  core/clock.cpp
  core/storage.cpp
  core/low_battery_monitor.cpp
  core/critical_battery_monitor.cpp
  core/network_status_monitor.cpp
  core/power_manager.cpp
  core/weather_provider.cpp
  core/weather_routes.cpp
  core/admin_auth_service.cpp
  core/diagnostics_routes.cpp
  core/ota_routes.cpp
  core/settings_routes.cpp
  core/logger.cpp
  core/notification_sound.cpp
  # Lives in ui/ (it's a UI-formatting concern, called from
  # ClockWidget/StatusBar), but is genuinely LVGL-free - built here
  # instead of homedeck_ui below so it's covered by tests/, which never
  # fetches LVGL. homedeck_ui already links homedeck_core, so this is
  # still reachable identically from every existing call site.
  ui/time_format.cpp
)
target_link_libraries(homedeck_core PUBLIC homedeck_platform_host mbedcrypto)

# Only defined when a target named `lvgl` already exists in this
# directory's scope - i.e. only for consumers (simulator/) that fetched
# LVGL before adding this directory. tests/ never fetches LVGL, so it
# simply doesn't get this target, keeping Core's unit tests LVGL-free.
if(TARGET lvgl)
  add_library(homedeck_ui
    ui/ui_dispatch.cpp
    ui/navigation.cpp
    ui/home_affordance.cpp
    ui/status_bar.cpp
    ui/dashboard_grid.cpp
    ui/clock_widget.cpp
    ui/network_status_widget.cpp
    ui/weather_widget.cpp
    ui/notification_banner.cpp
    ui/notification_widget.cpp
    ui/quick_settings_panel.cpp
    ui/keyboard_input.cpp
    ui/screens/dashboard_screen.cpp
    ui/screens/wifi_setup_screen.cpp
    ui/lvgl_user_activity_source.cpp
    ui/app_core.cpp
    # None of these three is a portable UI file - HostAudioOutput
    # (platform/host/audio_output.h, SDL2), UiTask
    # (platform/host/ui_task.h, SDL2), and HostDisplayBrightness
    # (platform/host/display_brightness.h, LVGL only, no SDL2) live here
    # rather than homedeck_platform_host below deliberately: that
    # library is shared with tests/, which never fetches LVGL/SDL2 and
    # must stay buildable without either. This target already links
    # both transitively (via lvgl's own SDL display driver, what
    # ui_task.cpp uses), so it's the natural home for LVGL/SDL2
    # consumers rather than adding a new dependency anywhere.
    platform/host/audio_output.cpp
    platform/host/ui_task.cpp
    platform/host/display_brightness.cpp
  )
  target_link_libraries(homedeck_ui PUBLIC homedeck_core lvgl)
endif()

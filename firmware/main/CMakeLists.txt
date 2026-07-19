# Reuses the portable src/core and src/ui/screens/dashboard_screen source
# directly (relative paths) rather than nesting src/CMakeLists.txt's
# plain-CMake add_subdirectory build inside this ESP-IDF component - the
# two build systems have different conventions (idf_component_register
# vs. add_library/target_link_libraries) and integrating them cleanly
# (in particular, confirming LVGL's `TARGET lvgl` check in
# src/CMakeLists.txt resolves correctly against the managed `lvgl`
# component) is real, unverified risk not worth taking on for this
# step. This does mean these paths need updating by hand if src/'s own
# file list changes - a known tradeoff, not an oversight.
idf_component_register(
    SRCS
        "homedeck.cpp"
        "wifi_setup.cpp"
        "crash_diagnostics.cpp"
        "../../src/core/event_bus.cpp"
        "../../src/core/clock.cpp"
        "../../src/core/storage.cpp"
        "../../src/core/low_battery_monitor.cpp"
        "../../src/core/admin_auth_service.cpp"
        "../../src/ui/screens/dashboard_screen.cpp"
        "../../src/ui/status_bar.cpp"
        "../../src/ui/dashboard_grid.cpp"
        "../../src/ui/notification_banner.cpp"
        "../../src/platform/firmware/task.cpp"
        "../../src/platform/firmware/timer.cpp"
        "../../src/platform/firmware/i2c_device.cpp"
        "../../src/platform/firmware/battery_reader.cpp"
        "../../src/platform/firmware/time_source.cpp"
        "../../src/platform/firmware/settings_store.cpp"
        "../../src/platform/firmware/secret_store.cpp"
        "../../src/platform/firmware/cache_store.cpp"
        "../../src/platform/firmware/http_server.cpp"
        "../../src/platform/static_assets.cpp"
    INCLUDE_DIRS
        "."
        "../../src"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        mdns
        nvs_flash
        fatfs
        wear_levelling
        espcoredump
        mbedtls
    EMBED_FILES
        "../../webui/index.html"
)

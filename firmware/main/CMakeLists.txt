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

# EMBED_FILES needs these to exist at CMake configure time, and the Vite
# build is a deliberately separate, explicit step (not auto-invoked from
# CMake - see
# docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage),
# so a stale/missing build here is a real, easy-to-hit case, not a
# hypothetical - fail with a clear message rather than EMBED_FILES'
# own opaque "file not found" error.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../webui/dist/index.html" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../webui/dist/app.js" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../webui/dist/app.css")
    message(FATAL_ERROR
        "webui/dist/ is missing or incomplete - run 'npm ci && npm run build' "
        "in webui/ before building firmware (see DEVELOPMENT.md).")
endif()

idf_component_register(
    SRCS
        "homedeck.cpp"
        "wifi_setup.cpp"
        "crash_diagnostics.cpp"
        "../../src/core/event_bus.cpp"
        "../../src/core/clock.cpp"
        "../../src/core/storage.cpp"
        "../../src/core/low_battery_monitor.cpp"
        "../../src/core/network_status_monitor.cpp"
        "../../src/core/weather_provider.cpp"
        "../../src/core/weather_routes.cpp"
        "../../src/core/admin_auth_service.cpp"
        "../../src/core/diagnostics_routes.cpp"
        "../../src/core/ota_routes.cpp"
        "../../src/core/settings_routes.cpp"
        "../../src/core/logger.cpp"
        "../../src/ui/screens/dashboard_screen.cpp"
        "../../src/ui/screens/wifi_setup_screen.cpp"
        "../../src/ui/status_bar.cpp"
        "../../src/ui/dashboard_grid.cpp"
        "../../src/ui/clock_widget.cpp"
        "../../src/ui/network_status_widget.cpp"
        "../../src/ui/weather_widget.cpp"
        "../../src/ui/notification_banner.cpp"
        "../../src/ui/navigation.cpp"
        "../../src/ui/home_affordance.cpp"
        "../../src/ui/keyboard_input.cpp"
        "../../src/platform/firmware/task.cpp"
        "../../src/platform/firmware/timer.cpp"
        "../../src/platform/firmware/i2c_device.cpp"
        "../../src/platform/firmware/battery_reader.cpp"
        "../../src/platform/firmware/network_status.cpp"
        "../../src/platform/firmware/time_source.cpp"
        "../../src/platform/firmware/settings_store.cpp"
        "../../src/platform/firmware/secret_store.cpp"
        "../../src/platform/firmware/cache_store.cpp"
        "../../src/platform/firmware/http_server.cpp"
        "../../src/platform/firmware/http_client.cpp"
        "../../src/platform/static_assets.cpp"
    INCLUDE_DIRS
        "."
        "../../src"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_http_client
        esp-tls
        mdns
        nvs_flash
        fatfs
        wear_levelling
        espcoredump
        spi_flash
        mbedtls
        app_update
    EMBED_FILES
        "../../webui/dist/index.html"
        "../../webui/dist/app.js"
        "../../webui/dist/app.css"
)

cmake_minimum_required(VERSION 3.16)
project(homedeck_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# The portable Core/UI source shared with simulator/ and firmware/ (see
# docs/architecture/overview.md#hardware-abstraction). LVGL is never
# fetched here, so src/'s homedeck_ui target isn't defined - only
# homedeck_core (and its homedeck_platform_host dependency) are.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_BINARY_DIR}/src)

enable_testing()
include(GoogleTest)

add_executable(homedeck_tests
  smoke_test.cpp
  task_test.cpp
  queue_test.cpp
  timer_test.cpp
  event_bus_test.cpp
  battery_reader_test.cpp
  clock_test.cpp
  time_format_test.cpp
  text_format_test.cpp
  grid_occupancy_test.cpp
  wifi_reconnect_policy_test.cpp
  retry_backoff_test.cpp
  wifi_credentials_test.cpp
  url_codec_test.cpp
  storage_test.cpp
  http_server_test.cpp
  http_client_test.cpp
  websocket_client_test.cpp
  static_assets_test.cpp
  latched_threshold_monitor_test.cpp
  low_battery_monitor_test.cpp
  critical_battery_monitor_test.cpp
  network_status_monitor_test.cpp
  power_manager_test.cpp
  display_brightness_test.cpp
  weather_provider_test.cpp
  harmony_connection_test.cpp
  harmony_notification_bridge_test.cpp
  notification_sound_test.cpp
  admin_auth_service_test.cpp
  admin_auth_routes_test.cpp
  diagnostics_routes_test.cpp
  ota_gate_test.cpp
  ota_routes_test.cpp
  settings_routes_test.cpp
  weather_routes_test.cpp
  harmony_routes_test.cpp
  wifi_routes_test.cpp
  logger_test.cpp
  activity_start_tracker_test.cpp
  command_button_press_tracker_test.cpp
)
target_link_libraries(homedeck_tests PRIVATE homedeck_core GTest::gtest_main GTest::gmock)

gtest_discover_tests(homedeck_tests)

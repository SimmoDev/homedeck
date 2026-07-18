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

add_library(homedeck_platform_host
  platform/host/task.cpp
  platform/host/timer.cpp
  platform/host/file_backed_store.cpp
  platform/host/settings_store.cpp
  platform/host/cache_store.cpp
  platform/host/http_server.cpp
)
target_include_directories(homedeck_platform_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(homedeck_platform_host PUBLIC cxx_std_20)
target_link_libraries(homedeck_platform_host PUBLIC civetweb-c-library)

add_library(homedeck_core
  core/event_bus.cpp
  core/clock.cpp
  core/storage.cpp
)
target_link_libraries(homedeck_core PUBLIC homedeck_platform_host)

# Only defined when a target named `lvgl` already exists in this
# directory's scope - i.e. only for consumers (simulator/) that fetched
# LVGL before adding this directory. tests/ never fetches LVGL, so it
# simply doesn't get this target, keeping Core's unit tests LVGL-free.
if(TARGET lvgl)
  add_library(homedeck_ui
    ui/ui_task.cpp
    ui/navigation.cpp
    ui/home_affordance.cpp
    ui/status_bar.cpp
    ui/screens/dashboard_screen.cpp
  )
  target_link_libraries(homedeck_ui PUBLIC homedeck_core lvgl)
endif()

cmake_minimum_required(VERSION 3.16)
project(homedeck_simulator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.5.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

# LVGL's SDL driver source needs SDL2's headers/libs at compile time; the
# FetchContent-provided lvgl target doesn't expose a way to inject that
# itself (see lvgl/lvgl#6957), so link it here directly.
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

# The portable Core/UI source shared with firmware/ (see
# docs/architecture/overview.md#hardware-abstraction) - added after LVGL
# so its homedeck_ui target (conditional on `lvgl` existing) is defined.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_BINARY_DIR}/src)

# The Vite build is a deliberately separate, explicit step (not
# auto-invoked from CMake - see
# docs/decisions/ADR-0025-webui-static-asset-storage.md),
# so a stale/missing build here is easy to hit, not hypothetical - fail
# at configure time with a clear message rather than main.cpp's
# ReadFile() silently warning and serving nothing at runtime.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../webui/dist/index.html" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../webui/dist/app.js" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../webui/dist/app.css")
  message(FATAL_ERROR
    "webui/dist/ is missing or incomplete - run 'npm ci && npm run build' "
    "in webui/ before building the simulator (see DEVELOPMENT.md).")
endif()

add_executable(homedeck_simulator main.cpp debug_panel.cpp)
target_link_libraries(homedeck_simulator PRIVATE homedeck_ui PkgConfig::SDL2)

# Source-tree-relative, not a relative path resolved at runtime against
# the simulator's working directory - see main.cpp's Web UI wiring.
target_compile_definitions(homedeck_simulator PRIVATE
  HOMEDECK_WEBUI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../webui/dist")

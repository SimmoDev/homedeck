add_library(homedeck_platform_host
  platform/host/task.cpp
  platform/host/timer.cpp
)
target_include_directories(homedeck_platform_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(homedeck_platform_host PUBLIC cxx_std_20)

add_library(homedeck_core
  core/event_bus.cpp
  core/clock.cpp
)
target_link_libraries(homedeck_core PUBLIC homedeck_platform_host)

# Only defined when a target named `lvgl` already exists in this
# directory's scope - i.e. only for consumers (simulator/) that fetched
# LVGL before adding this directory. tests/ never fetches LVGL, so it
# simply doesn't get this target, keeping Core's unit tests LVGL-free.
if(TARGET lvgl)
  add_library(homedeck_ui
    ui/ui_task.cpp
    ui/screens/dashboard_screen.cpp
  )
  target_link_libraries(homedeck_ui PUBLIC homedeck_core lvgl)
endif()

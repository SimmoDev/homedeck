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

enable_testing()
include(GoogleTest)

add_executable(homedeck_tests smoke_test.cpp)
target_link_libraries(homedeck_tests PRIVATE GTest::gtest_main GTest::gmock)

gtest_discover_tests(homedeck_tests)

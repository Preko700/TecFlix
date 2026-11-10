cmake_minimum_required(VERSION 3.22)
project(tecflix_streaming_api CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../common common)  # define TECFLIX_COVERAGE y tecflix_common

add_library(streaming_core STATIC
  src/streaming.cpp src/ffmpeg_transcoder.cpp src/metadata_client.cpp)
target_include_directories(streaming_core PUBLIC src)
target_link_libraries(streaming_core PUBLIC tecflix_common)

add_executable(streaming-api src/main.cpp)
target_link_libraries(streaming-api PRIVATE streaming_core)

enable_testing()
find_package(GTest REQUIRED)
add_executable(streaming_tests tests/streaming_test.cpp)
target_link_libraries(streaming_tests PRIVATE streaming_core GTest::gtest_main)
add_test(NAME streaming_tests COMMAND streaming_tests)

cmake_minimum_required(VERSION 3.22)
project(tecflix_metadata_api CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../common common)  # define TECFLIX_COVERAGE y tecflix_common
find_package(mongoc 2.0 REQUIRED)

add_library(metadata_core STATIC
  src/routes.cpp src/mongo_repository.cpp src/streaming_client.cpp)
target_include_directories(metadata_core PUBLIC src)
target_link_libraries(metadata_core PUBLIC tecflix_common PRIVATE mongoc::mongoc)

add_executable(metadata-api src/main.cpp)
target_link_libraries(metadata-api PRIVATE metadata_core)

enable_testing()
find_package(GTest REQUIRED)
add_executable(metadata_tests tests/metadata_test.cpp)
target_link_libraries(metadata_tests PRIVATE metadata_core GTest::gtest_main)
add_test(NAME metadata_tests COMMAND metadata_tests)

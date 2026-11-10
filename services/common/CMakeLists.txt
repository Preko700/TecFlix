cmake_minimum_required(VERSION 3.22)
project(tecflix_common CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TECFLIX_COVERAGE "Instrumentar para medir cobertura (gcovr)" OFF)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(httplib REQUIRED IMPORTED_TARGET cpp-httplib)

add_library(tecflix_common STATIC src/jwt.cpp)
target_include_directories(tecflix_common PUBLIC include)
target_compile_options(tecflix_common PUBLIC -Wall -Wextra)
target_link_libraries(tecflix_common PUBLIC
  OpenSSL::Crypto nlohmann_json::nlohmann_json PkgConfig::httplib Threads::Threads)

if(TECFLIX_COVERAGE)
  # PUBLIC: los servicios que enlazan la librería también quedan instrumentados.
  target_compile_options(tecflix_common PUBLIC --coverage -O0)
  target_link_options(tecflix_common PUBLIC --coverage)
endif()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(common_tests tests/common_test.cpp)
  target_link_libraries(common_tests PRIVATE tecflix_common GTest::gtest_main)
  add_test(NAME common_tests COMMAND common_tests)
endif()

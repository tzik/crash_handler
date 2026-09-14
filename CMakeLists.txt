cmake_minimum_required(VERSION 3.16)
project(CrashHandler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED IMPORTED_TARGET libelf)

find_package(absl REQUIRED)
find_package(nlohmann_json REQUIRED)

include_directories(include)

add_library(crash_util OBJECT src/util.cc)

add_library(crash_handler src/crash_handler.cc)
target_link_libraries(crash_handler PUBLIC absl::stacktrace crash_util)
target_include_directories(crash_handler PUBLIC include)

add_executable(crash_handler_worker src/worker.cc)
target_link_libraries(crash_handler_worker PRIVATE nlohmann_json::nlohmann_json PkgConfig::LIBELF crash_util absl::absl_check)

add_executable(test_crash src/test_crash.cc)
target_link_libraries(test_crash PRIVATE crash_handler)

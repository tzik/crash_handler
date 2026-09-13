cmake_minimum_required(VERSION 3.16)
project(CrashHandler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(absl REQUIRED)
find_package(nlohmann_json REQUIRED)

include_directories(include)

# The crash handler library
add_library(crash_handler src/crash_handler.cpp)
target_link_libraries(crash_handler PUBLIC absl::stacktrace absl::symbolize)
target_include_directories(crash_handler PUBLIC include)

# The worker executable
add_executable(crash_handler_worker src/worker.cpp)
target_link_libraries(crash_handler_worker PRIVATE nlohmann_json::nlohmann_json elf)

# A test executable
add_executable(test_crash src/test_crash.cpp)
target_link_libraries(test_crash PRIVATE crash_handler)

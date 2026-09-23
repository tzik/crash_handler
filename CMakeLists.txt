cmake_minimum_required(VERSION 3.16)
project(CrashHandler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_library(crash_util OBJECT src/util.cc)

add_library(crash_handler src/crash_handler.cc $<TARGET_OBJECTS:crash_util>)
target_compile_definitions(crash_handler PRIVATE CRASH_HANDLER_IMPLEMENTATION)
target_include_directories(crash_handler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

llvm_map_components_to_libnames(llvm_libs Symbolize Object Support)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <linux/fs.h>
#include <sys/ioctl.h>
int main() {
    struct procmap_query q;
    q.size = sizeof(q);
    int fd = 0;
    ioctl(fd, PROCMAP_QUERY, &q);
    return 0;
}
" HAVE_PROCMAP_QUERY)

add_executable(crash_handler_worker src/worker.cc $<TARGET_OBJECTS:crash_util>)
if(HAVE_PROCMAP_QUERY)
    target_compile_definitions(crash_handler_worker PRIVATE HAVE_PROCMAP_QUERY)
endif()
target_include_directories(crash_handler_worker PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(crash_handler_worker PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(crash_handler_worker PRIVATE
  ${llvm_libs})

add_executable(test_crash src/test_crash.cc)
target_link_libraries(test_crash PRIVATE crash_handler)

# Installation
install(TARGETS crash_handler crash_handler_worker
    EXPORT CrashHandlerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT CrashHandlerTargets
    FILE CrashHandlerTargets.cmake
    NAMESPACE CrashHandler::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrashHandler
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CrashHandlerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CrashHandlerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrashHandler
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/CrashHandlerConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrashHandler
)

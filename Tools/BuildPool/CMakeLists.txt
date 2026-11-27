# BuildPool -- one pool of compiler and linker slots per machine (README.md). A project of its own: C++20, the
# standard library and the OS API only, no header of any client project. It is never part of a client's build: a
# client's configure builds it once per source revision through cmake/BuildPoolBootstrap.cmake and puts the installed
# binary in front of every compile and link (cmake/BuildPool.cmake, buildpool_enable).
#
#   cmake -S Tools/BuildPool -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <dir>
cmake_minimum_required(VERSION 3.21)
project(BuildPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# One target. The sources are the four parts of the tool plus the OS seam of THIS platform: Source/Platform/*.cpp is
# shared by every OS; Windows/, or Posix/ plus the one memory probe of the kernel (Mac/, Linux/).
file(GLOB_RECURSE _buildpool_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Daemon/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Wire/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Client/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Invocation/*.cpp")
file(GLOB _buildpool_platform CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Platform/*.cpp")
if(WIN32)
    set(_buildpool_os_dirs Windows)
elseif(APPLE)
    set(_buildpool_os_dirs Posix Mac)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_buildpool_os_dirs Posix Linux)
else()
    message(FATAL_ERROR "BuildPool: no OS seam for ${CMAKE_SYSTEM_NAME} (Source/Platform has Windows, Posix+Mac, Posix+Linux)")
endif()
foreach(_dir IN LISTS _buildpool_os_dirs)
    file(GLOB_RECURSE _os CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Platform/${_dir}/*.cpp")
    list(APPEND _buildpool_platform ${_os})
endforeach()

add_executable(buildpool ${_buildpool_sources} ${_buildpool_platform})
target_include_directories(buildpool PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source")

if(MSVC)
    # The static runtime: the binary is copied into the pool home and must run beside any toolchain.
    set_property(TARGET buildpool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(buildpool PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    # The Windows macros (NOMINMAX, WIN32_LEAN_AND_MEAN) belong to Source/Platform/Windows/Win.hpp, the one includer of windows.h.
    # BCrypt (SHA-256), PSAPI (process memory); kernel32 holds the rest (Job Objects, pipes, memory status).
    target_link_libraries(buildpool PRIVATE bcrypt psapi)
else()
    target_compile_options(buildpool PRIVATE -Wall -Wextra)
endif()

install(TARGETS buildpool RUNTIME DESTINATION .)

# Protobuf 21.12 (the last release without an Abseil dependency): the wire-format runtime GameNetworkingSockets'
# messages are generated against, and protoc, the host tool that generates them. Upstream's own CMakeLists.txt is
# added here, unlike the other vendored libraries: protobuf's source lists are ~400 files maintained in its
# cmake/*.cmake, its target names (libprotobuf, libprotoc, protoc) collide with nothing of ours, and protoc must be
# the exact build of the runtime it generates for. Nothing is installed and no test, example or zlib is built.
set(_root "${DESERT_ROOT}/ThirdParty/protobuf")
if(NOT EXISTS "${_root}/CMakeLists.txt")
    message(FATAL_ERROR "ThirdParty/protobuf is empty. It is a submodule: run `git submodule update --init ThirdParty/protobuf`.")
endif()
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBPROTOC ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory("${_root}" "${CMAKE_BINARY_DIR}/ThirdParty/protobuf" EXCLUDE_FROM_ALL SYSTEM)
foreach(_t libprotobuf libprotobuf-lite libprotoc protoc)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER ThirdParty/protobuf)
        desert_warnings_off(${_t})
        if(NOT DESERT_IS_WINDOWS)
            set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        endif()
    endif()
endforeach()

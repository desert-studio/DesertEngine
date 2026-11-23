# Valve GameNetworkingSockets: the transport under Desert/Net's INetTransport (Docs/contracts/NET-ARCH.md §1) --
# reliable + unreliable messages, fragmentation, congestion control, AES-GCM encryption always on, and the
# lag/loss simulator every NET scenario runs under. Built as one static library "GameNetworkingSockets" from
# upstream's own source lists (src/CMakeLists.txt GNS_*_SRCS), without ICE/WebRTC (dedicated servers, connect by
# IP:port) and without Steam relay.
# Dependencies: protobuf (cmake/ThirdParty/Protobuf.cmake, built here, its protoc generates the messages) and a
# crypto backend: BCrypt on Windows (part of the OS); OpenSSL >= 1.1.1 elsewhere, linked statically. OpenSSL is
# found, not vendored: it builds through Perl/Configure, not CMake, and on a Linux host it is the distro's
# security-patched libssl-dev; on macOS it is Homebrew's openssl@3.
set(_root "${DESERT_ROOT}/ThirdParty/GameNetworkingSockets")
if(NOT EXISTS "${_root}/include/steam/steamnetworkingsockets.h")
    message(FATAL_ERROR "ThirdParty/GameNetworkingSockets is empty. It is a submodule: run `git submodule update --init ThirdParty/GameNetworkingSockets`.")
endif()
set(_src "${_root}/src")

# Generated protobuf messages: protoc (the one built from ThirdParty/protobuf) writes them into the build tree.
set(_gen "${CMAKE_BINARY_DIR}/ThirdParty/GameNetworkingSockets/proto")
file(MAKE_DIRECTORY "${_gen}")
set(_proto_sources "")
foreach(_name steamnetworkingsockets_messages_certs steamnetworkingsockets_messages steamnetworkingsockets_messages_udp)
    add_custom_command(
        OUTPUT "${_gen}/${_name}.pb.cc" "${_gen}/${_name}.pb.h"
        COMMAND protoc "--cpp_out=${_gen}" "--proto_path=${_src}/common" "${_src}/common/${_name}.proto"
        DEPENDS protoc "${_src}/common/${_name}.proto"
        COMMENT "protoc: GameNetworkingSockets ${_name}.proto"
        VERBATIM)
    list(APPEND _proto_sources "${_gen}/${_name}.pb.cc")
endforeach()

set(_sources
    common/steamid.cpp
    steamnetworkingsockets/steamnetworkingsockets_certs.cpp
    steamnetworkingsockets/steamnetworkingsockets_certstore.cpp
    steamnetworkingsockets/steamnetworkingsockets_shared.cpp
    tier0/dbg.cpp
    tier0/platformtime.cpp
    tier0/valve_tracelogging.cpp
    tier1/netadr.cpp
    tier1/utlbuffer.cpp
    tier1/utlmemory.cpp
    tier1/ipv6text.c
    vstdlib/strtools.cpp
    steamnetworkingsockets/steamnetworkingsockets_stats.cpp
    steamnetworkingsockets/steamnetworkingsockets_thinker.cpp
    steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp
    steamnetworkingsockets/clientlib/csteamnetworkingmessages.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_socketthread.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel_misc.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_spew.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_taskqueue.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p_ice.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p_webrtc.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp
    steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp
    common/crypto.cpp
    common/crypto_textencode.cpp
    common/keypair.cpp)
# STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS: the certificate store trusts a self-signed root added at run
# time - the mesh CA (Desert/Net MeshCertificates.cpp) - instead of only Valve's hardcoded Steam root, which our own
# servers have no certificates from. Only that file adds roots; client connections stay as they were.
set(_defines VALVE_CRYPTO_ENABLE_25519 GOOGLE_PROTOBUF_NO_RTTI STEAMNETWORKINGSOCKETS_FOREXPORT ENABLE_OPENSSLCONNECTION
    STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS)
if(DESERT_IS_WINDOWS)
    list(APPEND _sources
        common/crypto_bcrypt.cpp
        common/crypto_25519_donna.cpp
        external/curve25519-donna/curve25519.c
        external/curve25519-donna/curve25519_VALVE_sse2.c
        external/ed25519-donna/ed25519_VALVE.c
        external/ed25519-donna/ed25519_VALVE_sse2.c)
    list(APPEND _defines VALVE_CRYPTO_BCRYPT VALVE_CRYPTO_25519_DONNA ED25519_HASH_BCRYPT
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    if(DESERT_IS_MACOS AND NOT OPENSSL_ROOT_DIR AND EXISTS "/opt/homebrew/opt/openssl@3")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    endif()
    set(OPENSSL_USE_STATIC_LIBS ON)
    find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
    list(APPEND _sources
        common/crypto_openssl.cpp
        common/crypto_25519_openssl.cpp
        common/crypto_digest_opensslevp.cpp
        common/crypto_symmetric_opensslevp.cpp
        common/opensslwrapper.cpp)
    list(APPEND _defines VALVE_CRYPTO_OPENSSL VALVE_CRYPTO_25519_OPENSSLEVP)
endif()
list(TRANSFORM _sources PREPEND "${_src}/")

desert_add_library(GameNetworkingSockets FOLDER ThirdParty SOURCES ${_sources} ${_proto_sources})
desert_warnings_off(GameNetworkingSockets)
set_target_properties(GameNetworkingSockets PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON C_STANDARD 99)
target_compile_definitions(GameNetworkingSockets PRIVATE ${_defines})
# Static link: the public headers must not declare the API as dllimport/visibility-exported.
target_compile_definitions(GameNetworkingSockets PUBLIC STEAMNETWORKINGSOCKETS_STATIC_LINK)
target_include_directories(GameNetworkingSockets SYSTEM PUBLIC "${_root}/include")
target_include_directories(GameNetworkingSockets PRIVATE "${_src}" "${_src}/common" "${_src}/public" "${_gen}")
target_link_libraries(GameNetworkingSockets PUBLIC libprotobuf)
if(DESERT_IS_WINDOWS)
    target_compile_definitions(GameNetworkingSockets PUBLIC _WINDOWS)
    target_compile_options(GameNetworkingSockets PRIVATE /EHs-c- $<IF:$<CONFIG:Debug>,/GR,/GR->)
    target_link_libraries(GameNetworkingSockets PUBLIC ws2_32 crypt32 winmm Iphlpapi bcrypt)
else()
    if(DESERT_IS_MACOS)
        target_compile_definitions(GameNetworkingSockets PUBLIC OSX)
        target_link_libraries(GameNetworkingSockets PUBLIC "-framework CoreFoundation")
    else()
        target_compile_definitions(GameNetworkingSockets PUBLIC LINUX)
    endif()
    target_compile_options(GameNetworkingSockets PRIVATE
        -fvisibility=hidden -fno-strict-aliasing -ffunction-sections -fdata-sections -fstack-protector-strong
        $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden -fno-exceptions -fno-rtti>)
    set_target_properties(GameNetworkingSockets PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
    target_link_libraries(GameNetworkingSockets PUBLIC OpenSSL::Crypto Threads::Threads)
endif()

# GNS's private headers and defines, for the one engine file that builds GNS certificates (Desert/Net
# MeshCertificates.cpp: the mesh CA of NET-ARCH §2M.2 uses GNS's own certificate store and Ed25519 keys).
set(DESERT_GNS_PRIVATE_INCLUDES "${_src}" "${_src}/common" "${_src}/public" "${_gen}" CACHE INTERNAL "")
set(DESERT_GNS_PRIVATE_DEFINES ${_defines} CACHE INTERNAL "")

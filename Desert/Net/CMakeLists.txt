# DesertNet: the network transport layer (Docs/contracts/NET-ARCH.md §1) -- INetTransport and its
# GameNetworkingSockets implementation, plus the connect handshake. Device-free and engine-free: the dedicated
# server (DesertServer), the client runtime and the editor's PIE all link the same library.
desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_library(DesertNet FOLDER Engine SOURCES ${_sources})
target_include_directories(DesertNet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(DesertNet PRIVATE GameNetworkingSockets)
desert_platform_defines(DesertNet)
# The mesh CA (MeshCertificates.cpp) signs and trusts GNS certificates with GNS's own internals: that file alone
# sees GNS's private headers and crypto defines, and stays out of unity batches so they reach no other file.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Source/Net/MeshCertificates.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${DESERT_GNS_PRIVATE_INCLUDES}"
    COMPILE_DEFINITIONS "${DESERT_GNS_PRIVATE_DEFINES}"
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON)

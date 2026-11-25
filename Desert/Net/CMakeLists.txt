# DesertNet: the network transport layer (Docs/contracts/NET-ARCH.md §1) -- INetTransport and its
# GameNetworkingSockets implementation, plus the connect handshake. Device-free and engine-free: the dedicated
# server (DesertServer), the client runtime and the editor's PIE all link the same library.
desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_library(DesertNet FOLDER Engine SOURCES ${_sources})
target_include_directories(DesertNet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(DesertNet PRIVATE GameNetworkingSockets)
desert_platform_defines(DesertNet)
# The mesh CA (MeshCertificates.cpp) signs and trusts GNS certificates, and the connect token (ConnectToken.cpp)
# signs and verifies its Ed25519 envelope, with GNS's own internals: those files alone see GNS's private headers
# and crypto defines, and stay out of unity batches so they reach no other file.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Source/Net/MeshCertificates.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Net/ConnectToken.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${DESERT_GNS_PRIVATE_INCLUDES}"
    COMPILE_DEFINITIONS "${DESERT_GNS_PRIVATE_DEFINES}"
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON)

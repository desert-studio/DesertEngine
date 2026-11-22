# DesertNet: the network transport layer (Docs/contracts/NET-ARCH.md §1) -- INetTransport and its
# GameNetworkingSockets implementation, plus the connect handshake. Device-free and engine-free: the dedicated
# server (DesertServer), the client runtime and the editor's PIE all link the same library.
desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_library(DesertNet FOLDER Engine SOURCES ${_sources})
target_include_directories(DesertNet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(DesertNet PRIVATE GameNetworkingSockets)
desert_platform_defines(DesertNet)

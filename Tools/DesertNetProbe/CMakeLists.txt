# DesertNetProbe: proves the transport, its lanes and the token handshake on its own (NET-1 scenario
# net1b_lanes_token) -- one binary: --issue-token, --server or --client, under the GNS lag/loss simulator.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(DesertNetProbe FOLDER Tools SOURCES ${_sources})
target_link_libraries(DesertNetProbe PRIVATE DesertNet)
# The cook hash of a cooked world directory: Engine/Core/Serialize/CookHash.hpp, header-only and dependency-free
# (the probe links no engine module).
target_include_directories(DesertNetProbe PRIVATE "${DESERT_ROOT}/Desert/Desert/Source")
desert_platform_defines(DesertNetProbe)

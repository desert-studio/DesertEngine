# DesertNetProbe: proves the transport + handshake on its own (NET-1 scenario net1a_handshake) -- one binary,
# --server or --client, exchanging numbered reliable messages under the GNS lag/loss simulator.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(DesertNetProbe FOLDER Tools SOURCES ${_sources})
target_link_libraries(DesertNetProbe PRIVATE DesertNet)
desert_platform_defines(DesertNetProbe)

# Spdlog: spdlog 1.x compiled with its bundled fmt (SPDLOG_COMPILED_LIB in every target: cmake/DesertWorkspace.cmake).
# One registry and one default logger per process: a DLL outside Shipping (PROCESS_STATE; SPDLOG_SHARED_LIB and
# FMT_SHARED in every target, the export side here), static in the Shipping monolith. Common links it PUBLIC: its
# Logger.hpp is in every PCH. ThirdParty/spdlog's own CMakeLists.txt is not used (its options, its install rules).
set(_root "${DESERT_ROOT}/ThirdParty/spdlog")
set(_src async.cpp bundled_fmtlib_format.cpp cfg.cpp color_sinks.cpp file_sinks.cpp spdlog.cpp stdout_sinks.cpp)
list(TRANSFORM _src PREPEND "${_root}/src/")
desert_add_library(spdlog PROCESS_STATE FOLDER ThirdParty SOURCES ${_src})
target_include_directories(spdlog PRIVATE "${_root}/include")
if(NOT DESERT_MONOLITHIC)
    set_target_properties(spdlog PROPERTIES DEFINE_SYMBOL "spdlog_EXPORTS")
    target_compile_definitions(spdlog PRIVATE FMT_LIB_EXPORT)
endif()

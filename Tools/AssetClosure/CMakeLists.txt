# AssetClosure: the asset dependency closure over the editor's AssetReferences, LINKED from the editor libraries
# (desert_link_editor_tool), never compiled a second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(AssetClosure FOLDER Tools SOURCES ${_sources})
target_include_directories(AssetClosure PRIVATE "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Editor/Source")
desert_engine_includes(AssetClosure)
desert_external_includes(AssetClosure ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(AssetClosure)
desert_link_editor_tool(AssetClosure)

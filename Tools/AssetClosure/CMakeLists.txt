# AssetClosure: the asset dependency closure; compiles the editor's AssetReferences TUs
# straight in, links Common. No DESERT_PLATFORM_LINUX: only Windows and macOS are built.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(AssetClosure FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Editor/Source/Editor/Core/AssetReferences.cpp"
    "${DESERT_ROOT}/Editor/Source/Editor/Core/AssetReferencesScan.cpp")
target_include_directories(AssetClosure PRIVATE
    "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Desert/Common/Source" "${DESERT_ROOT}/Editor/Source")
desert_external_includes(AssetClosure ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_COMMONSPECIFIC_INCLUDE_DIRS})
target_link_libraries(AssetClosure PRIVATE Common ReflectCpp)
if(DESERT_IS_WINDOWS)
    target_compile_definitions(AssetClosure PRIVATE DESERT_PLATFORM_WINDOWS)
elseif(DESERT_IS_MACOS)
    target_compile_definitions(AssetClosure PRIVATE DESERT_PLATFORM_MACOS)
endif()

# Mimalloc: the process allocator of the executables that ask for it (DESERT_MIMALLOC, desert_link_mimalloc).
# UE: FMallocMimalloc / FMallocBinned picked as the process allocator at startup. mimalloc v2 as mimalloc.dll with
# the redirection module beside it: mimalloc-redirect.dll (prebuilt, shipped in the submodule's bin/) patches the
# dynamic C runtime's malloc/free/realloc at load, so EVERY module of the process -- the engine, the vendored
# libraries, the driver's user-mode part -- allocates from mimalloc, not only the code that calls mi_malloc.
# ThirdParty/mimalloc's own CMakeLists.txt is not used: it declares mimalloc, mimalloc-static, mimalloc-obj and its
# tests, and names the library after CMAKE_BUILD_TYPE (mimalloc-debug.dll), which ReadProcessAllocator
# (Common/Utilities/ProcessMemory.cpp) and the packager would have to guess. Here: one DLL, always mimalloc.dll.
# The upstream settings it reproduces (CMakeLists.txt of v2.5.2): MI_OVERRIDE ON, MI_WIN_REDIRECT ON, the sources
# compiled as C++ under MSVC (MI_USE_CXX DEFAULT -> ON: C++ atomics), MI_BUILD_RELEASE outside Debug.
if(NOT DESERT_MIMALLOC)
    return()
endif()
set(_root "${DESERT_ROOT}/ThirdParty/mimalloc")
if(NOT EXISTS "${_root}/include/mimalloc.h" OR NOT EXISTS "${_root}/bin/mimalloc-redirect.dll")
    message(FATAL_ERROR "ThirdParty/mimalloc is empty. It is a submodule: run "
                        "`git submodule update --init ThirdParty/mimalloc` "
                        "(or configure with -DDESERT_MIMALLOC=OFF).")
endif()
# src/static.c is the whole library in one translation unit (it includes every other source), as upstream's
# mimalloc-obj builds it.
set(_src "${_root}/src/static.c")
set_source_files_properties("${_src}" PROPERTIES LANGUAGE CXX)
add_library(Mimalloc SHARED "${_src}")
desert_own_isa_dispatch(Mimalloc)
set_target_properties(Mimalloc PROPERTIES FOLDER ThirdParty OUTPUT_NAME mimalloc)
target_include_directories(Mimalloc PUBLIC "${_root}/include")
target_compile_definitions(Mimalloc PRIVATE
    MI_SHARED_LIB MI_SHARED_LIB_EXPORT MI_MALLOC_OVERRIDE $<$<NOT:$<CONFIG:Debug>>:MI_BUILD_RELEASE>)
target_compile_options(Mimalloc PRIVATE /Zc:__cplusplus)
# The redirection module's import library: mimalloc.dll imports mimalloc-redirect.dll, which is what the loader
# must find beside it (copied below, and by the packager with the executable's import closure).
target_link_libraries(Mimalloc PRIVATE psapi shell32 user32 advapi32 bcrypt "${_root}/bin/mimalloc-redirect.lib")
add_custom_command(TARGET Mimalloc POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_root}/bin/mimalloc-redirect.dll" "$<TARGET_FILE_DIR:Mimalloc>"
    COMMENT "Mimalloc: mimalloc-redirect.dll beside mimalloc.dll")

# Assimp: assimp from the Editor/ThirdParty/assimp submodule, built with ONLY the
# importers BuildScripts/ThirdParty/AssimpImporters.txt names (every other ASSIMP_BUILD_NO_<X>_IMPORTER defined),
# no exporter, bundled zlib/unzip. The headers assimp's own CMake configure would write (config.h, revision.h,
# zconf.h, VERSION.txt) are written into build/generated/assimp at configure time.
# Assimp's own CMakeLists.txt is not used: it builds every importer and exporter, and adds zlib/unzip/test targets.
set(_root "${DESERT_ROOT}/Editor/ThirdParty/assimp")
set(_generated "${DESERT_ROOT}/build/generated/assimp/include")
set(_register "${DESERT_ROOT}/BuildScripts/ThirdParty/AssimpImporters.txt")
if(NOT EXISTS "${_root}/code/CMakeLists.txt")
    message(FATAL_ERROR "Editor/ThirdParty/assimp is empty. It is a submodule now (it used to be a committed "
                        "prebuilt): run `git submodule update --init --recursive`.")
endif()
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_register}" "${_root}/code/Common/ImporterRegistry.cpp" "${_root}/include/assimp/config.h.in"
    "${_root}/include/assimp/revision.h.in" "${_root}/CMakeLists.txt")

# Every importer guard assimp's registry knows.
if(NOT EXISTS "${_root}/code/Common/ImporterRegistry.cpp")
    message(FATAL_ERROR "assimp's code/Common/ImporterRegistry.cpp is missing; the set of importers to disable "
                        "cannot be derived and the build would ship every format assimp has.")
endif()
file(READ "${_root}/code/Common/ImporterRegistry.cpp" _registry)
string(REGEX MATCHALL "ASSIMP_BUILD_NO_[A-Z0-9_]+_IMPORTER" _guards "${_registry}")
list(TRANSFORM _guards REPLACE "^ASSIMP_BUILD_NO_(.*)_IMPORTER$" "\\1")
list(REMOVE_DUPLICATES _guards)

# The importers to keep: the first word of each non-comment line of the register. GLTF covers three guards.
# Read as text and split on newlines: file(STRINGS) would also split a description at its ';'.
file(READ "${_register}" _lines)
string(REPLACE ";" "," _lines "${_lines}")
string(REPLACE "" "" _lines "${_lines}")
string(REPLACE "
" ";" _lines "${_lines}")
set(_keep "")
set(_count 0)
foreach(line IN LISTS _lines)
    if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
        continue()
    endif()
    if(line MATCHES "^[ \t]*([A-Za-z0-9_]+)")
        set(_name "${CMAKE_MATCH_1}")
        math(EXPR _count "${_count} + 1")
        if(NOT _name IN_LIST _guards)
            message(FATAL_ERROR "AssimpImporters.txt names importer '${_name}', which assimp's "
                                "code/Common/ImporterRegistry.cpp does not guard. Check the spelling against its "
                                "ASSIMP_BUILD_NO_<NAME>_IMPORTER lines.")
        endif()
        if(_name STREQUAL "GLTF")
            list(APPEND _keep GLTF GLTF1 GLTF2)
        else()
            list(APPEND _keep "${_name}")
        endif()
    endif()
endforeach()
if(_count EQUAL 0)
    message(FATAL_ERROR "BuildScripts/ThirdParty/AssimpImporters.txt names no importers. An assimp with zero "
                        "importers builds and parses nothing; say which formats you want.")
endif()
set(_disable "")
foreach(name IN LISTS _guards)
    if(NOT name IN_LIST _keep)
        list(APPEND _disable "ASSIMP_BUILD_NO_${name}_IMPORTER")
    endif()
endforeach()
list(SORT _disable)

# The generated headers.
file(READ "${_root}/include/assimp/config.h.in" _config)
string(REPLACE "#cmakedefine ASSIMP_DOUBLE_PRECISION 1" "/* #undef ASSIMP_DOUBLE_PRECISION */" _config "${_config}")
desert_write_generated("${_generated}/assimp/config.h" "${_config}")
file(STRINGS "${_root}/CMakeLists.txt" _project_line REGEX "^[ \t]*PROJECT[ \t]*\\([ \t]*Assimp[ \t]+VERSION[ \t]+[0-9]+\\.[0-9]+\\.[0-9]+")
if(NOT _project_line MATCHES "VERSION[ \t]+([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "could not read assimp's version from its PROJECT() line; revision.h would claim 0.0.0.")
endif()
set(_major ${CMAKE_MATCH_1})
set(_minor ${CMAKE_MATCH_2})
set(_patch ${CMAKE_MATCH_3})
file(READ "${_root}/include/assimp/revision.h.in" _revision)
foreach(pair "@GIT_COMMIT_HASH@|0" "@GIT_BRANCH@|pinned-submodule" "@ASSIMP_VERSION_MAJOR@|${_major}"
             "@ASSIMP_VERSION_MINOR@|${_minor}" "@ASSIMP_VERSION_PATCH@|${_patch}" "@ASSIMP_PACKAGE_VERSION@|0"
             "@CMAKE_SHARED_LIBRARY_PREFIX@|" "@LIBRARY_SUFFIX@|" "@CMAKE_DEBUG_POSTFIX@|")
    string(FIND "${pair}" "|" _bar)
    string(SUBSTRING "${pair}" 0 ${_bar} _from)
    math(EXPR _bar "${_bar} + 1")
    string(SUBSTRING "${pair}" ${_bar} -1 _to)
    string(REPLACE "${_from}" "${_to}" _revision "${_revision}")
endforeach()
desert_write_generated("${_generated}/assimp/revision.h" "${_revision}")
if(NOT EXISTS "${_root}/contrib/zlib/zconf.h.included")
    message(FATAL_ERROR "assimp's contrib/zlib/zconf.h.included is missing; zlib cannot be configured and the "
                        "build would silently read the system zlib's zconf.h instead.")
endif()
file(READ "${_root}/contrib/zlib/zconf.h.included" _zconf)
desert_write_generated("${_generated}/zconf.h" "${_zconf}")
desert_write_generated("${DESERT_ROOT}/build/generated/assimp/VERSION.txt" "${_major}.${_minor}.${_patch}\n")

desert_glob(_src
    "${_root}/code/Common/*.cpp" "${_root}/code/CApi/*.cpp" "${_root}/code/Geometry/*.cpp"
    "${_root}/code/Material/*.cpp" "${_root}/code/PostProcessing/*.cpp" "${_root}/code/AssetLib/FBX/*.cpp"
    "${_root}/code/AssetLib/Obj/*.cpp" "${_root}/code/AssetLib/glTFCommon/*.cpp" "${_root}/code/AssetLib/glTF/*.cpp"
    "${_root}/code/AssetLib/glTF2/*.cpp")
file(GLOB _contrib CONFIGURE_DEPENDS "${_root}/contrib/zlib/*.c" "${_root}/contrib/unzip/*.c")
list(SORT _contrib)
desert_add_library(Assimp FOLDER ThirdParty SOURCES ${_src} ${_contrib})
set_target_properties(Assimp PROPERTIES CXX_STANDARD 17)
target_include_directories(Assimp PRIVATE
    "${_root}" "${_root}/code" "${_root}/include" "${_generated}" "${_root}/contrib" "${_root}/contrib/zlib"
    "${_root}/contrib/unzip" "${_root}/contrib/rapidjson/include" "${_root}/contrib/utf8cpp/source")
target_compile_definitions(Assimp PRIVATE ASSIMP_BUILD_NO_EXPORT RAPIDJSON_HAS_STDSTRING ${_disable})
if(DESERT_IS_WINDOWS)
    target_compile_definitions(Assimp PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
else()
    set_target_properties(Assimp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(Assimp PRIVATE Z_HAVE_UNISTD_H)
endif()
if(NOT MSVC)
    target_compile_options(Assimp PRIVATE -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
                                          -Wno-deprecated-declarations -Wno-implicit-fallthrough)
endif()

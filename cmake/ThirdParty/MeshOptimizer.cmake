# BuildScripts/ThirdParty/MeshOptimizer.lua: meshoptimizer v1.3, every source under src/.
# Its own CMakeLists.txt is not used: it declares `meshoptimizer` (another name) plus gltfpack/demo options.
set(_root "${DESERT_ROOT}/ThirdParty/meshoptimizer")
if(NOT EXISTS "${_root}/src/meshoptimizer.h")
    message(FATAL_ERROR "ThirdParty/meshoptimizer sources are missing. Run scripts/MacOS/Setup.sh (macOS) "
                        "or clone https://github.com/zeux/meshoptimizer into ThirdParty/meshoptimizer.")
endif()
# A stale v0.20 checkout compiles and asserts at runtime; refuse it at configure time, as premake did.
file(READ "${_root}/src/meshoptimizer.h" _header)
string(FIND "${_header}" "#define MESHOPTIMIZER_VERSION 1030" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "ThirdParty/meshoptimizer is not v1.3. Re-run scripts/MacOS/Setup.sh (macOS) or "
                        "scripts\Windows\Setup.bat (Windows); they re-fetch a mismatched version.")
endif()
desert_glob(_src "${_root}/src/*.cpp" "${_root}/src/*.h")
desert_add_library(MeshOptimizer FOLDER ThirdParty SOURCES ${_src})
set_target_properties(MeshOptimizer PROPERTIES CXX_STANDARD 17)
target_include_directories(MeshOptimizer PRIVATE "${_root}/src")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(MeshOptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

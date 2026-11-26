# Luau: Luau 0.741 as ONE library `Luau` built from six object libraries (VM, Compiler, CodeGen and their
# Ast/Bytecode/Common), each C++17, headers under <module>/include, sources <module>/src/*.cpp (not recursive).
# The VM keeps process state that a lua_State points at (the shared empty-table node, the nil object compared by
# address), and two module DLLs run it on one state (Core: LuauRuntime/LuauBinder, ScriptLuau: LuauHostNatives):
# so it is a PROCESS_STATE library (cmake/DesertWorkspace.cmake THIRD-PARTY PROCESS STATE), a DLL outside Shipping
# that exports every symbol (its C API and the C++ Luau::CodeGen calls the engine makes carry no export macro).
set(_root "${DESERT_ROOT}/ThirdParty/luau")
if(NOT EXISTS "${_root}/VM/include/lua.h")
    message(FATAL_ERROR "ThirdParty/luau sources are missing. Run `git submodule update --init ThirdParty/luau`.")
endif()
function(_desert_luau name module)
    _desert_thirdparty_glob(_headers RECURSE "${_root}/${module}/include/*.h")
    _desert_thirdparty_glob(_src "${_root}/${module}/src/*.cpp" "${_root}/${module}/src/*.h")
    add_library(Luau${name} OBJECT ${_headers} ${_src})
    set_target_properties(Luau${name} PROPERTIES FOLDER ThirdParty)
    desert_warnings_off(Luau${name})
    set_target_properties(Luau${name} PROPERTIES CXX_STANDARD 17)
    set(_inc "")
    foreach(dir IN LISTS ARGN)
        list(APPEND _inc "${_root}/${dir}")
    endforeach()
    target_include_directories(Luau${name} PRIVATE ${_inc})
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(Luau${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        set_target_properties(Luau${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endfunction()
_desert_luau(Common Common Common/include)
_desert_luau(Ast Ast Ast/include Common/include)
_desert_luau(Bytecode Bytecode Bytecode/include Common/include)
_desert_luau(Compiler Compiler Compiler/include Ast/include Bytecode/include Common/include)
_desert_luau(VM VM VM/include Common/include)
if(DESERT_IS_WINDOWS)
    # Upstream's own setting: MSVC's SSA PRE pass makes the interpreter loop slower.
    set_property(SOURCE "${_root}/VM/src/lvmexecute.cpp" APPEND PROPERTY COMPILE_OPTIONS /d2ssa-pre-)
else()
    target_compile_options(LuauVM PRIVATE -fno-math-errno)
endif()
_desert_luau(CodeGen CodeGen CodeGen/include VM/include VM/src Common/include)
desert_add_library(Luau PROCESS_STATE FOLDER ThirdParty SOURCES
    $<TARGET_OBJECTS:LuauCommon> $<TARGET_OBJECTS:LuauAst> $<TARGET_OBJECTS:LuauBytecode>
    $<TARGET_OBJECTS:LuauCompiler> $<TARGET_OBJECTS:LuauVM> $<TARGET_OBJECTS:LuauCodeGen>)
set_target_properties(Luau PROPERTIES LINKER_LANGUAGE CXX WINDOWS_EXPORT_ALL_SYMBOLS ON)

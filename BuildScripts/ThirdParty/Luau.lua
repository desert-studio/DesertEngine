-- Luau (luau-lang/luau, MIT; ThirdParty/luau/LICENSE.txt and lua_LICENSE.txt, listed in
-- ThirdParty/THIRD_PARTY_LICENSES.md). Pinned submodule at the 0.741 release tag.
--
-- Why it is here: the gameplay scripting layer moves from PUC Lua 5.4 to Luau — a sandboxable VM with a
-- bytecode compiler, gradual types and a native code generator (Notion wiki "Скриптинг", §3/§6).
--
-- Six static libraries, mirroring upstream's CMake targets one-to-one (CMakeLists.txt + Sources.cmake);
-- the upstream CMake is NOT run. Analysis, Config, Require, Inliner and CLI are not compiled: the engine
-- embeds the VM and the compiler, it does not ship the type checker or the REPL.
--
--   LuauCommon   <- (nothing)
--   LuauAst      <- Common
--   LuauBytecode <- Common
--   LuauCompiler <- Ast, Bytecode, Common
--   LuauVM       <- Common
--   LuauCodeGen  <- VM (+ its private VM/src headers), Common
--
-- LUA_API keeps upstream's default (C++ linkage, LUAU_EXTERN_C off): every lua_* symbol is mangled, so
-- these libraries can sit in one link with the PUC Lua project while the move is in progress.
--
-- Compiler flags are upstream's (CMakeLists.txt LUAU_OPTIONS): _CRT_SECURE_NO_WARNINGS on MSVC (the
-- portable CRT functions are used on purpose), /d2ssa-pre- on lvmexecute.cpp (MSVC's partial
-- redundancy elimination regresses the interpreter loop), -fno-math-errno on the VM elsewhere (lets
-- sqrt() lower to one instruction). Upstream's warning set is its authors' style; we compile it quiet.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/luau"

if not os.isfile( root .. "/VM/include/lua.h" ) then
    error( "ThirdParty/luau sources are missing. Run `git submodule update --init ThirdParty/luau`." )
end

local function LuauProject( name, module, includes )
    project( "Luau" .. name )
        kind "StaticLib"
        language "C++"
        cppdialect "C++17"
        location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "Luau" .. name )
        warnings "Off"

        files
        {
            root .. "/" .. module .. "/include/**.h",
            root .. "/" .. module .. "/src/*.cpp",
            root .. "/" .. module .. "/src/*.h",
        }

        includedirs( includes )

        filter "system:windows"
            systemversion "latest"
            defines { "_CRT_SECURE_NO_WARNINGS" }

        filter "system:not windows"
            pic "On"

        filter {}
end

LuauProject( "Common", "Common", { root .. "/Common/include" } )

LuauProject( "Ast", "Ast", { root .. "/Ast/include", root .. "/Common/include" } )
    links { "LuauCommon" }

LuauProject( "Bytecode", "Bytecode", { root .. "/Bytecode/include", root .. "/Common/include" } )
    links { "LuauCommon" }

LuauProject( "Compiler", "Compiler",
    { root .. "/Compiler/include", root .. "/Ast/include", root .. "/Bytecode/include", root .. "/Common/include" } )
    links { "LuauAst", "LuauBytecode", "LuauCommon" }

LuauProject( "VM", "VM", { root .. "/VM/include", root .. "/Common/include" } )
    links { "LuauCommon" }

    filter "system:not windows"
        buildoptions { "-fno-math-errno" }

    filter { "system:windows", "files:**/VM/src/lvmexecute.cpp" }
        buildoptions { "/d2ssa-pre-" }

    filter {}

LuauProject( "CodeGen", "CodeGen",
    { root .. "/CodeGen/include", root .. "/VM/include", root .. "/VM/src", root .. "/Common/include" } )
    links { "LuauVM", "LuauCommon" }

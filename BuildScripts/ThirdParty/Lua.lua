-- Lua static library, built from the ThirdParty/lua submodule sources.
-- lua.c (standalone interpreter), onelua.c (amalgamation) and ltests.c
-- (upstream test harness) are excluded: the engine embeds Lua via sol2.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/lua"

project "Lua"
    kind "StaticLib"
    language "C"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "Lua" )

    files
    {
        root .. "/*.c",
        root .. "/*.h",
    }

    removefiles
    {
        root .. "/lua.c",
        root .. "/onelua.c",
        root .. "/ltests.c",
    }

    -- Coexistence with Luau (BuildScripts/ThirdParty/Luau.lua) while the scripting layer moves over; deleted
    -- with this project. Luau's lua_* FUNCTIONS have C++ linkage, so they are mangled and never meet ours, but a
    -- namespace-scope VARIABLE keeps its plain name under the Itanium ABI (clang/gcc; MSVC mangles it): Luau's
    -- lapi.cpp defines `lua_ident` exactly as lapi.c does, and a link holding both is a duplicate symbol. Of the
    -- two archives' external symbols this is the only shared name (nm -g of libLua.a vs libLuau*.a). PUC's copy
    -- is renamed here because nothing reads it (lua.h declares it; no engine, sol2 or tool source names it).
    defines { "lua_ident=desert_puc_lua_ident" }
    -- gmake does not rebuild on a changed define, so an archive built before the rename would still link; a new
    -- archive name makes every tree build the renamed one instead of reusing the stale libLua.a.
    targetname "LuaPuc"

    filter "system:windows"
        systemversion "latest"

    filter "system:macosx"
        pic "On"
        defines { "LUA_USE_MACOSX" }

    filter "system:linux"
        pic "On"
        defines { "LUA_USE_LINUX" }

    filter {}

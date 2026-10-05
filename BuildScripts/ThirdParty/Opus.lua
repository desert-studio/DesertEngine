-- libopus (xiph/opus v1.5.2, BSD-3-Clause), the audio decoder behind Engine/Media (Opus in WebM).
-- COMPILED FROM SOURCE on every platform, like dav1d: a pinned submodule and a premake StaticLib.
--
-- THE FILE LISTS ARE THE SUBMODULE'S OWN: celt_sources.mk, silk_sources.mk and opus_sources.mk are the
-- lists opus's autotools and CMake builds both read, so a release that adds a file adds it here too.
-- Floating-point build (SILK_SOURCES_FLOAT / OPUS_SOURCES_FLOAT), generic C (no RTCD/NEON/SSE units),
-- no deep-learning PLC (ENABLE_DEEP_PLC off, its default). USE_ALLOCA is the one stack-array mode MSVC
-- supports (it has no VLAs).

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/opus"

if not os.isfile( root .. "/include/opus.h" ) then
    error( "ThirdParty/opus is empty. It is a submodule: run `git submodule update --init ThirdParty/opus`." )
end

local function MakeList( mk, name )
    local text = "\n" .. ( io.readfile( root .. "/" .. mk ) or "" ) .. "\n\n" -- a list may end the file
    local body = text:match( "\n" .. name .. " = \\\n(.-)\n%s*\n" )
    if not body then
        error( "Opus.lua: no list " .. name .. " in " .. root .. "/" .. mk )
    end
    local list = {}
    for path in body:gmatch( "([%w_/%.]+%.c)" ) do
        table.insert( list, root .. "/" .. path )
    end
    return list
end

project "Opus"
    kind "StaticLib"
    language "C"
    cdialect "C11"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/Opus" )

    files ( MakeList( "celt_sources.mk", "CELT_SOURCES" ) )
    files ( MakeList( "silk_sources.mk", "SILK_SOURCES" ) )
    files ( MakeList( "silk_sources.mk", "SILK_SOURCES_FLOAT" ) )
    files ( MakeList( "opus_sources.mk", "OPUS_SOURCES" ) )
    files ( MakeList( "opus_sources.mk", "OPUS_SOURCES_FLOAT" ) )

    includedirs { root .. "/include", root .. "/celt", root .. "/silk", root .. "/silk/float", root }
    defines { "OPUS_BUILD", "USE_ALLOCA", "HAVE_LRINTF" }

    -- Third-party code: its warnings are its authors' style, not ours to read.
    warnings "Off"

    -- OPTIMISED IN EVERY CONFIGURATION (UE builds its third-party media libraries the same way): a codec is
    -- not code anyone steps through, and at -O0 decoding an Opus packet falls behind the sound clock — a Debug
    -- game then drops every frame of its startup movie and shows the last, faded-out one (2026-10-05).
    optimize "Speed"

    filter "system:windows"
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:not windows"
        pic "On"

    filter {}

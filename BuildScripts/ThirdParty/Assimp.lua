-- assimp (assimp/assimp, 3-clause BSD), COMPILED FROM SOURCE on every platform.
--
-- WHAT THIS REPLACES, AND WHY THE REPLACEMENT IS NOT A PREFERENCE. Until this project existed the two
-- platforms did not build the same library at all: Windows linked a 400 KB import library committed on
-- 27 July, built with toolset v142 while the workspace pins v143, whose name (`assimp-vc142-mtd`) was
-- typed into the link line so an update broke the build somewhere that does not mention assimp; macOS
-- linked `-lassimp` out of Homebrew, i.e. whatever the developer happened to have — 6.0.5 on this
-- machine against the prebuilt's 5.x. NEITHER SIDE PINNED A VERSION, so two machines could build a
-- different engine from one commit, and the DLL half of the Windows pair was never committed at all, so
-- FbxMeshSplitter's postbuild copy silently did nothing on every Windows build (`if exist`).
--
-- The submodule is pinned at v6.0.5, which is also what Homebrew ships here: the macOS side of this
-- change is therefore version-neutral and observable rather than a leap.
--
-- ONE PRECISION, ONE ZLIB, ON BOTH PLATFORMS. assimp's CMake takes the system zlib when it finds one and
-- its own `contrib/zlib` otherwise — which on this tree means Homebrew's on macOS and the bundled one on
-- Windows, i.e. exactly the two-platform divergence this task exists to remove. `contrib/zlib` is
-- compiled here on both, so an inflate bug is the same inflate bug everywhere.

local root      = _MAIN_SCRIPT_DIR .. "/Editor/ThirdParty/assimp"
local generated = _MAIN_SCRIPT_DIR .. "/build/generated/assimp/include"
local register  = _MAIN_SCRIPT_DIR .. "/BuildScripts/ThirdParty/AssimpImporters.txt"

if not os.isfile( root .. "/code/CMakeLists.txt" ) then
    error( "Editor/ThirdParty/assimp is empty. It is a submodule now (it used to be a committed " ..
           "prebuilt): run `git submodule update --init --recursive`." )
end

-- THE FULL GUARD LIST COMES FROM THE FILE THAT ACTUALLY DECIDES, and the first version of this read the
-- wrong one. `code/CMakeLists.txt`'s `ADD_ASSIMP_IMPORTER( NAME )` lines look like the register of
-- formats, and they are not: `code/Common/ImporterRegistry.cpp` is what instantiates importers, its
-- guards are `#ifndef ASSIMP_BUILD_NO_<NAME>_IMPORTER`, and the two sets DISAGREE. C4D is guarded there
-- and declared nowhere in that macro list (its define is added by hand at CMakeLists.txt:695), so the
-- first build of this project linked and failed on `vtable for Assimp::C4DImporter` — a format we had
-- never heard of, referenced by a registry we thought we had covered. glTF is the same shape in the
-- other direction: three cooperating guards for one format.
--
-- So the authority is the registry's own guards. Reading them rather than typing them also means a
-- format a future assimp adds arrives DISABLED — the safe direction — instead of being shipped because
-- nobody updated a list.
local function AllGuardNames()
    local names  = {}
    local source = io.readfile( root .. "/code/Common/ImporterRegistry.cpp" )
    if not source then
        error( "assimp's code/Common/ImporterRegistry.cpp is missing; the set of importers to disable " ..
               "cannot be derived and the build would ship every format assimp has." )
    end
    for name in source:gmatch( "ASSIMP_BUILD_NO_([A-Z0-9_]+)_IMPORTER" ) do
        names[name] = true
    end
    return names
end

-- ONE FORMAT, THREE GUARDS. assimp gates glTF with an umbrella macro plus one per major version, and
-- all three have to stay undefined for `.gltf`/`.glb` to load. Written down here because a mapping that
-- lives only in somebody's head is how the register ends up lying about what ships.
local guardFamilies = {
    GLTF = { "GLTF", "GLTF1", "GLTF2" },
}

-- The rows of AssimpImporters.txt: NAME | extensions | reason. Comments and blank lines ignored.
local function EnabledImporterNames()
    local names = {}
    local count = 0
    for line in io.lines( register ) do
        if not line:match( "^%s*#" ) and not line:match( "^%s*$" ) then
            local name = line:match( "^%s*([A-Za-z0-9_]+)" )
            if name then
                names[name] = true
                count       = count + 1
            end
        end
    end
    if count == 0 then
        error( "BuildScripts/ThirdParty/AssimpImporters.txt names no importers. An assimp with zero " ..
               "importers builds and parses nothing, which is a silent empty success of the kind this " ..
               "tree has already paid for; say which formats you want." )
    end
    return names
end

local allNames       = AllGuardNames()
local registerNames  = EnabledImporterNames()
local disableDefines = {}
local keepGuard      = {}

-- A row naming a format assimp does not guard is a typo that would otherwise disable NOTHING extra and
-- ship everything, so it is an error here rather than a surprise in the census.
for name in pairs( registerNames ) do
    if not allNames[name] then
        error( "AssimpImporters.txt names importer '" .. name .. "', which assimp's " ..
               "code/Common/ImporterRegistry.cpp does not guard. Check the spelling against its " ..
               "ASSIMP_BUILD_NO_<NAME>_IMPORTER lines." )
    end
    for _, guard in ipairs( guardFamilies[name] or { name } ) do
        keepGuard[guard] = true
    end
end

for name in pairs( allNames ) do
    if not keepGuard[name] then
        table.insert( disableDefines, "ASSIMP_BUILD_NO_" .. name .. "_IMPORTER" )
    end
end
table.sort( disableDefines )

-- config.h AND revision.h ARE GENERATED HERE, FROM THE SUBMODULE'S OWN `.in` FILES.
--
-- The alternative was to commit both headers. config.h.in is 1187 lines with exactly ONE substitution in
-- it (`#cmakedefine ASSIMP_DOUBLE_PRECISION`), so a committed copy would be 1187 lines that must agree
-- with the submodule and nothing in the tree would check that they do — the drift shape this project
-- keeps paying for. Generating keeps one source of truth and costs a premake pass, which every build
-- already needs (the makefiles do not exist until premake runs).
local function GenerateHeaders()
    os.mkdir( generated .. "/assimp" )

    local configIn = io.readfile( root .. "/include/assimp/config.h.in" )
    if not configIn then
        error( "assimp's include/assimp/config.h.in is missing; the submodule checkout is incomplete." )
    end
    -- Single precision: aiVector3D etc. stay float, which is what every engine struct that copies them
    -- assumes. Written as the comment CMake would leave, so the header reads like a configured one.
    configIn = configIn:gsub( "#cmakedefine ASSIMP_DOUBLE_PRECISION 1",
                              "/* #undef ASSIMP_DOUBLE_PRECISION */" )
    io.writefile( generated .. "/assimp/config.h", configIn )

    -- The version is read out of assimp's own PROJECT() line, so the number this engine reports and the
    -- number the pinned source carries cannot disagree. Desert/Tests/Editor/AssimpBoundary asserts the
    -- third side of it: what the BUILT library answers through aiGetVersion*.
    local major, minor, patch
    for line in io.lines( root .. "/CMakeLists.txt" ) do
        local a, b, c = line:match( "^%s*PROJECT%s*%(%s*Assimp%s+VERSION%s+(%d+)%.(%d+)%.(%d+)" )
        if a then
            major, minor, patch = a, b, c
            break
        end
    end
    if not major then
        error( "could not read assimp's version from its PROJECT() line; revision.h would claim 0.0.0 " ..
               "and the version census would pass on a lie." )
    end

    local revisionIn = io.readfile( root .. "/include/assimp/revision.h.in" )
    revisionIn = revisionIn:gsub( "@GIT_COMMIT_HASH@", "0" )
                           :gsub( "@GIT_BRANCH@", "pinned-submodule" )
                           :gsub( "@ASSIMP_VERSION_MAJOR@", major )
                           :gsub( "@ASSIMP_VERSION_MINOR@", minor )
                           :gsub( "@ASSIMP_VERSION_PATCH@", patch )
                           :gsub( "@ASSIMP_PACKAGE_VERSION@", "0" )
                           :gsub( "@CMAKE_SHARED_LIBRARY_PREFIX@", "" )
                           :gsub( "@LIBRARY_SUFFIX@", "" )
                           :gsub( "@CMAKE_DEBUG_POSTFIX@", "" )
    io.writefile( generated .. "/assimp/revision.h", revisionIn )

    -- AND zlib's zconf.h, WHICH IS THE SAME KIND OF GENERATED HEADER AND WAS THE FIRST THING TO BREAK.
    -- contrib/zlib ships zconf.h.in / .cmakein / .included but NO zconf.h — zlib's own CMake configures
    -- one. Without it, `#include "zconf.h"` from contrib/zlib/zlib.h falls through to the macOS SDK's
    -- copy, so assimp's zlib.h and zutil.h were being read against a DIFFERENT zlib's configuration:
    -- `adler32.c` defines adler32_combine64 with z_off64_t while zutil.h had declared it with z_off_t,
    -- and clang refused with "conflicting types". Two halves of one library, each individually correct.
    -- `zconf.h.included` is the stock, already-configured header zlib keeps for exactly this purpose,
    -- and it lands in the generated dir, which precedes the SDK on the include path.
    local zconf = io.readfile( root .. "/contrib/zlib/zconf.h.included" )
    if not zconf then
        error( "assimp's contrib/zlib/zconf.h.included is missing; zlib cannot be configured and the " ..
               "build would silently read the system zlib's zconf.h instead." )
    end
    io.writefile( generated .. "/zconf.h", zconf )

    -- The one number the whole pin rests on, written where a test can read it without a compiler and
    -- without CMake. See Desert/Tests/Editor/AssimpBoundary.
    io.writefile( _MAIN_SCRIPT_DIR .. "/build/generated/assimp/VERSION.txt",
                  major .. "." .. minor .. "." .. patch .. "\n" )
end

GenerateHeaders()

project "Assimp"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/Assimp" )

    -- Only the directories the three registered formats need. The rest of code/AssetLib is not compiled
    -- AND is switched off by define, which are two different statements: the defines are what keep
    -- code/Common/ImporterRegistry.cpp from referring to classes that are not here.
    files {
        root .. "/code/Common/**.cpp",
        root .. "/code/CApi/**.cpp",
        root .. "/code/Geometry/**.cpp",
        root .. "/code/Material/**.cpp",
        root .. "/code/PostProcessing/**.cpp",
        root .. "/code/AssetLib/FBX/**.cpp",
        root .. "/code/AssetLib/Obj/**.cpp",
        root .. "/code/AssetLib/glTFCommon/**.cpp",
        root .. "/code/AssetLib/glTF/**.cpp",
        root .. "/code/AssetLib/glTF2/**.cpp",
        root .. "/contrib/zlib/*.c",
        -- ZipArchiveIOSystem.cpp lives in code/Common and needs minizip's unzip.h. Its only consumers
        -- are Collada, 3MF and Q3BSP — all disabled — but it is compiled rather than excluded: an
        -- exclusion is a second list to keep in step with assimp's own, and two .c files cost less than
        -- that.
        root .. "/contrib/unzip/*.c",
    }

    includedirs {
        root,
        root .. "/code",
        root .. "/include",
        generated,
        root .. "/contrib",
        root .. "/contrib/zlib",
        root .. "/contrib/unzip",
        root .. "/contrib/rapidjson/include",
        root .. "/contrib/utf8cpp/source",
    }

    defines {
        "ASSIMP_BUILD_NO_EXPORT",  -- nothing in this engine writes a model through assimp
        "RAPIDJSON_HAS_STDSTRING",
    }
    defines ( disableDefines )

    filter "system:windows"
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS", "WIN32_LEAN_AND_MEAN" }

    filter "system:not windows"
        pic "On"
        -- What zlib's own CMake decides with CheckIncludeFile(unistd.h). The stock zconf.h ships it
        -- UNSET (`/* #undef Z_HAVE_UNISTD_H */`), so zlib's gzip file API compiled against no
        -- declaration for read/write/lseek/close — which clang 16+ rejects outright rather than
        -- guessing, and which older compilers accepted by implicitly declaring them as int(...).
        defines { "Z_HAVE_UNISTD_H" }

    filter "toolset:clang or gcc"
        -- Third-party code, compiled with the workspace's -Wall -Wextra. These are assimp's own style,
        -- not defects in it, and drowning our own warnings is how a real one gets missed.
        buildoptions { "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                       "-Wno-deprecated-declarations", "-Wno-implicit-fallthrough" }

    filter {}

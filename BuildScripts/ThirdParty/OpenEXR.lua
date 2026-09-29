-- OpenEXR (AcademySoftwareFoundation/openexr, BSD-3-Clause), COMPILED FROM SOURCE on every platform,
-- the same way assimp is: a pinned submodule and a premake StaticLib, no CMake on the build.
--
-- ONLY OpenEXRCore IS BUILT. It is OpenEXR's C library (`exr_*`): headers, every compression codec
-- (NONE/RLE/ZIPS/ZIP/PIZ/PXR24/B44/B44A/DWAA/DWAB/HTJ2K/ZSTD) and the decode pipeline. The C++ layer
-- (OpenEXR/Iex/IlmThread) is not needed to read a texture and is not compiled. It replaced tinyexr,
-- which cannot decode DWAA/DWAB — the compression Poly Haven ships its normal maps in.
--
-- WHAT IT PULLS IN, AND FROM WHERE. Every codec library comes from the openexr submodule's own
-- `external/` directory, at the versions OpenEXR pins there (external/current_*_version), so nothing
-- here chooses a second version of anything:
--   * libdeflate — `external/deflate`, compiled by being #included into compression.c
--                  (OPENEXR_USE_INTERNAL_DEFLATE), exactly as OpenEXR's own CMake does it;
--   * zstd       — `external/zstd/lib`, the file list OpenEXRCore's CMakeLists.txt names;
--   * OpenJPH    — `external/OpenJPH/src/core`, for HTJ2K only.
-- Imath is the one outside dependency, and only for `half.h` (IMATH_HALF_NO_LOOKUP_TABLE: header-only).
--
-- SIMD. zstd's hand-written amd64 assembly is off (ZSTD_DISABLE_ASM) and OpenJPH is built without its
-- per-file SIMD variants (OJPH_DISABLE_SIMD): those need per-file -mavx2 / /arch:AVX2 switches and a
-- CPU dispatch that is only worth it for HTJ2K, which no asset in this tree uses. DWA/PIZ/ZIP keep
-- their own SIMD paths (they are selected at run time inside OpenEXRCore).

local root      = _MAIN_SCRIPT_DIR .. "/ThirdParty/openexr"
local imath     = _MAIN_SCRIPT_DIR .. "/ThirdParty/Imath"
local core      = root .. "/src/lib/OpenEXRCore"
local zstd      = root .. "/external/zstd/lib"
local ojph      = root .. "/external/OpenJPH/src/core"
local generated = _MAIN_SCRIPT_DIR .. "/build/generated/openexr/include"

if not os.isfile( core .. "/openexr.h" ) or not os.isfile( imath .. "/src/Imath/half.h" ) then
    error( "ThirdParty/openexr or ThirdParty/Imath is empty. Both are submodules: run " ..
           "`git submodule update --init --recursive`." )
end

-- THE CONFIG HEADERS CMAKE WOULD WRITE, written from the submodules' own templates so a new release's
-- new field arrives as an unsubstituted `@NAME@` compile error rather than a silently wrong value.
local function ReadDefine( path, name )
    local text = io.readfile( path )
    local value = text and text:match( "#%s*define%s+" .. name .. "%s+(%d+)" )
    if not value then
        error( "OpenEXR.lua: cannot read " .. name .. " from " .. path )
    end
    return value
end

local function Substitute( template, values, cmakedefines )
    local text = io.readfile( template )
    if not text then
        error( "OpenEXR.lua: template missing: " .. template )
    end
    text = text:gsub( "#cmakedefine01%s+([%w_]+)", function( name )
        return "#define " .. name .. " " .. ( cmakedefines[name] and "1" or "0" )
    end )
    text = text:gsub( "#cmakedefine%s+([%w_]+)[^\n]*", function( name )
        if cmakedefines[name] then
            return "#define " .. name .. " 1"
        end
        return "/* #undef " .. name .. " */"
    end )
    text = text:gsub( "@([%w_]+)@", function( name )
        local value = values[name]
        if value == nil then
            error( "OpenEXR.lua: no value for @" .. name .. "@ in " .. template )
        end
        return value
    end )
    return text
end

do
    local exrMajor   = ReadDefine( core .. "/openexr_version.h", "OPENEXR_VERSION_MAJOR" )
    local exrMinor   = ReadDefine( core .. "/openexr_version.h", "OPENEXR_VERSION_MINOR" )
    local exrPatch   = ReadDefine( core .. "/openexr_version.h", "OPENEXR_VERSION_PATCH" )
    local ojphMajor  = ReadDefine( ojph .. "/openjph/ojph_version.h", "OPENJPH_VERSION_MAJOR" )
    local ojphMinor  = ReadDefine( ojph .. "/openjph/ojph_version.h", "OPENJPH_VERSION_MINOR" )
    local ojphPatch  = ReadDefine( ojph .. "/openjph/ojph_version.h", "OPENJPH_VERSION_PATCH" )
    local zstdMajor  = ReadDefine( zstd .. "/zstd.h", "ZSTD_VERSION_MAJOR" )
    local zstdMinor  = ReadDefine( zstd .. "/zstd.h", "ZSTD_VERSION_MINOR" )
    local zstdPatch  = ReadDefine( zstd .. "/zstd.h", "ZSTD_VERSION_RELEASE" )
    local imathMajor = "3"
    local imathTag   = io.readfile( imath .. "/CMakeLists.txt" ):match( "project%s*%(%s*Imath%s+VERSION%s+([%d%.]+)" )
    if not imathTag then
        error( "OpenEXR.lua: cannot read Imath's version from ThirdParty/Imath/CMakeLists.txt" )
    end
    local imathMinor, imathPatch = imathTag:match( "^%d+%.(%d+)%.(%d+)" )
    local exrVersion  = exrMajor .. "." .. exrMinor .. "." .. exrPatch

    local isDarwin  = os.target() == "macosx"
    local isWindows = os.target() == "windows"

    local values = {
        OPENEXR_VERSION_MAJOR = exrMajor, OPENEXR_VERSION_MINOR = exrMinor, OPENEXR_VERSION_PATCH = exrPatch,
        OPENEXR_LIB_SOVERSION = "0",
        Imath_SOVERSION = "0", Imath_VERSION_MAJOR = imathMajor, Imath_VERSION_MINOR = imathMinor,
        Imath_VERSION_PATCH = imathPatch,
        openjph_VERSION_MAJOR = ojphMajor, openjph_VERSION_MINOR = ojphMinor, openjph_VERSION_PATCH = ojphPatch,
        zstd_VERSION_MAJOR = zstdMajor, zstd_VERSION_MINOR = zstdMinor, zstd_VERSION_PATCH = zstdPatch,
        OPENEXR_NAMESPACE_CUSTOM = "0", OPENEXR_INTERNAL_IMF_NAMESPACE = "Imf_3_5", OPENEXR_IMF_NAMESPACE = "Imf",
        OPENEXR_VERSION = exrVersion, OPENEXR_PACKAGE_NAME = "OpenEXR " .. exrVersion,
        OPENEXR_VERSION_RELEASE_TYPE = "", OPENEXR_LIB_VERSION = exrVersion,
        ILMTHREAD_NAMESPACE_CUSTOM = "0", ILMTHREAD_INTERNAL_NAMESPACE = "IlmThread_3_5",
        ILMTHREAD_NAMESPACE = "IlmThread",
        IMATH_NAMESPACE_CUSTOM = "0", IMATH_INTERNAL_NAMESPACE = "Imath_3_2", IMATH_NAMESPACE = "Imath",
        IMATH_VERSION = imathTag, IMATH_PACKAGE_NAME = "Imath " .. imathTag, IMATH_VERSION_RELEASE_TYPE = "",
        IMATH_LIB_VERSION = imathTag,
    }
    local defined = {
        OPENEXR_USE_INTERNAL_DEFLATE = true,
        OPENEXR_USE_INTERNAL_ZSTD    = true,
        OPENEXR_IMF_HAVE_DARWIN      = isDarwin,
        ILMTHREAD_THREADING_ENABLED  = true,
        IMATH_USE_NOEXCEPT           = true,
        -- Everything else stays off: no lookup-table half (header-only Imath), no API visibility
        -- (static library), no GCC inline AVX asm probe, no large-stack assumption.
    }
    if isWindows then
        defined.OPENEXR_IMF_HAVE_DARWIN = false
    end

    os.mkdir( generated .. "/Imath" )
    local cmake = root .. "/cmake"
    io.writefile( generated .. "/OpenEXRConfig.h", Substitute( cmake .. "/OpenEXRConfig.h.in", values, defined ) )
    io.writefile( generated .. "/OpenEXRConfigInternal.h",
                  Substitute( cmake .. "/OpenEXRConfigInternal.h.in", values, defined ) )
    io.writefile( generated .. "/IlmThreadConfig.h", Substitute( cmake .. "/IlmThreadConfig.h.in", values, defined ) )
    local imathConfig = Substitute( imath .. "/config/ImathConfig.h.in", values, defined )
    io.writefile( generated .. "/Imath/ImathConfig.h", imathConfig )
    io.writefile( generated .. "/ImathConfig.h", imathConfig )
end


project "OpenEXRCore"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    cdialect "C11"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/OpenEXRCore" )

    files {
        core .. "/*.c",
        core .. "/*.cpp",
        core .. "/*.h",

        zstd .. "/common/debug.c",
        zstd .. "/common/entropy_common.c",
        zstd .. "/common/error_private.c",
        zstd .. "/common/fse_decompress.c",
        zstd .. "/common/pool.c",
        zstd .. "/common/threading.c",
        zstd .. "/common/xxhash.c",
        zstd .. "/common/zstd_common.c",
        zstd .. "/compress/fse_compress.c",
        zstd .. "/compress/hist.c",
        zstd .. "/compress/huf_compress.c",
        zstd .. "/compress/zstd_compress.c",
        zstd .. "/compress/zstd_compress_literals.c",
        zstd .. "/compress/zstd_compress_sequences.c",
        zstd .. "/compress/zstd_compress_superblock.c",
        zstd .. "/compress/zstd_double_fast.c",
        zstd .. "/compress/zstd_fast.c",
        zstd .. "/compress/zstd_lazy.c",
        zstd .. "/compress/zstd_ldm.c",
        zstd .. "/compress/zstd_opt.c",
        zstd .. "/compress/zstd_preSplit.c",
        zstd .. "/compress/zstdmt_compress.c",
        zstd .. "/decompress/huf_decompress.c",
        zstd .. "/decompress/zstd_ddict.c",
        zstd .. "/decompress/zstd_decompress.c",
        zstd .. "/decompress/zstd_decompress_block.c",

        ojph .. "/codestream/*.cpp",
        ojph .. "/coding/*.cpp",
        ojph .. "/others/*.cpp",
        ojph .. "/others/*.c",
        ojph .. "/transform/*.cpp",
    }
    -- OpenJPH's per-instruction-set variants (see SIMD above); the generic files remain.
    removefiles {
        ojph .. "/**_sse.cpp", ojph .. "/**_sse2.cpp", ojph .. "/**_ssse3.cpp", ojph .. "/**_avx.cpp",
        ojph .. "/**_avx2.cpp", ojph .. "/**_avx512.cpp", ojph .. "/**_wasm.cpp", ojph .. "/**_vsx.cpp",
    }

    includedirs {
        core,
        generated,
        imath .. "/src/Imath",
        zstd,
        ojph,
        ojph .. "/openjph",
        ojph .. "/shared",
    }

    defines { "ZSTD_DISABLE_ASM", "OJPH_DISABLE_SIMD", "_FILE_OFFSET_BITS=64" }

    -- Third-party code: its warnings are its authors' style, not ours to read.
    warnings "Off"

    filter "system:windows"
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS", "WIN32_LEAN_AND_MEAN" }

    filter "system:not windows"
        pic "On"
        defines { "OJPH_POSIX_MEMALIGN_EXISTS" }

    filter {}

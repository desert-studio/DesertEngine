-- dav1d (videolan/dav1d 1.5.1, BSD-2-Clause), the AV1 decoder behind Engine/Media. COMPILED FROM SOURCE
-- on every platform, the way assimp and OpenEXR are: a pinned submodule and a premake StaticLib, no meson.
--
-- WHAT MESON WOULD GENERATE, AND HOW IT IS GENERATED HERE.
--   * config.h       — written below; the per-target answers are preprocessor tests, so ONE header serves
--                      clang/macOS and MSVC/Windows on arm64 and x86-64 alike;
--   * vcs_version.h  — DAV1D_VERSION, read from the submodule's own meson.build `version:` line;
--   * bitdepth units — meson compiles every `*_tmpl.c` twice, with BITDEPTH=8 and BITDEPTH=16. premake
--                      compiles a file once per project, so each (template, bitdepth) pair gets a two-line
--                      unit in build/generated/dav1d/ that defines BITDEPTH and #includes the template.
--                      Both bitdepths are built: a 10-bit AV1 stream is an ordinary encoder setting.
--
-- ASSEMBLY IS OFF (HAVE_ASM 0): dav1d's hand-written kernels are GNU-as `.S` (aarch64) and NASM `.asm`
-- (x86-64). premake's gmake and vs2022 generators have no assembler rule for either on both platforms,
-- and NASM is not part of the build toolchain. The C path decodes every stream bit-exactly; the speed
-- cost is recorded in the MEDIA-1 handover.

local root      = _MAIN_SCRIPT_DIR .. "/ThirdParty/dav1d"
local generated = _MAIN_SCRIPT_DIR .. "/build/generated/dav1d"

if not os.isfile( root .. "/include/dav1d/dav1d.h" ) then
    error( "ThirdParty/dav1d is empty. It is a submodule: run `git submodule update --init ThirdParty/dav1d`." )
end

local templates = {
    "cdef_apply_tmpl", "cdef_tmpl", "fg_apply_tmpl", "filmgrain_tmpl", "ipred_prepare_tmpl", "ipred_tmpl",
    "itx_tmpl", "lf_apply_tmpl", "loopfilter_tmpl", "looprestoration_tmpl", "lr_apply_tmpl", "mc_tmpl",
    "recon_tmpl",
}

do
    local version = io.readfile( root .. "/meson.build" ):match( "version%s*:%s*'([%d%.]+)'" )
    if not version then
        error( "Dav1d.lua: cannot read the version from " .. root .. "/meson.build" )
    end
    os.mkdir( generated )
    io.writefile( generated .. "/vcs_version.h",
        "/* written by BuildScripts/ThirdParty/Dav1d.lua */\n#define DAV1D_VERSION \"" .. version .. "\"\n" )
    io.writefile( generated .. "/config.h", [[
/* written by BuildScripts/ThirdParty/Dav1d.lua: the header meson's configure step would write */
#pragma once
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define CONFIG_LOG 1
#define CONFIG_MACOS_KPERF 0
#define HAVE_ASM 0
#define TRIM_DSP_FUNCTIONS 0
#define ENDIANNESS_BIG 0
#if defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_AARCH64 1
#else
#define ARCH_AARCH64 0
#endif
#define ARCH_ARM 0
#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86 1
#define ARCH_X86_64 1
#else
#define ARCH_X86 0
#define ARCH_X86_64 0
#endif
#define ARCH_X86_32 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define HAVE_AS_FUNC 0
#define HAVE_AS_ARCH_DIRECTIVE 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_DLSYM 0
#define HAVE_MEMALIGN 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_SYS_TYPES_H 1
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#define _CRT_DECLARE_NONSTDC_NAMES 1
#define HAVE_UNISTD_H 0
#define HAVE_IO_H 1
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_CLOCK_GETTIME 0
#else
#define HAVE_UNISTD_H 1
#define HAVE_IO_H 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_CLOCK_GETTIME 1
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define HAVE_C11_GENERIC 0
#else
#define HAVE_C11_GENERIC 1
#endif
]] )
    for _, name in ipairs( templates ) do
        for _, depth in ipairs( { "8", "16" } ) do
            io.writefile( generated .. "/" .. name .. "_" .. depth .. "bpc.c",
                "/* written by BuildScripts/ThirdParty/Dav1d.lua */\n#define BITDEPTH " .. depth ..
                "\n#include \"src/" .. name .. ".c\"\n" )
        end
    end
end

project "Dav1d"
    kind "StaticLib"
    language "C"
    cdialect "C11"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/Dav1d" )

    files {
        root .. "/src/cdf.c", root .. "/src/cpu.c", root .. "/src/ctx.c", root .. "/src/data.c",
        root .. "/src/decode.c", root .. "/src/dequant_tables.c", root .. "/src/getbits.c",
        root .. "/src/intra_edge.c", root .. "/src/itx_1d.c", root .. "/src/lf_mask.c", root .. "/src/lib.c",
        root .. "/src/log.c", root .. "/src/mem.c", root .. "/src/msac.c", root .. "/src/obu.c",
        root .. "/src/pal.c", root .. "/src/picture.c", root .. "/src/qm.c", root .. "/src/ref.c",
        root .. "/src/refmvs.c", root .. "/src/scan.c", root .. "/src/tables.c", root .. "/src/thread_task.c",
        root .. "/src/warpmv.c", root .. "/src/wedge.c",
        generated .. "/*_8bpc.c",
        generated .. "/*_16bpc.c",
    }

    -- `generated` FIRST: config.h and vcs_version.h are found there before anything in the submodule.
    includedirs { generated, root, root .. "/include", root .. "/include/dav1d" }

    -- Third-party code: its warnings are its authors' style, not ours to read.
    warnings "Off"

    -- OPTIMISED IN EVERY CONFIGURATION (UE builds its third-party media libraries the same way): a codec is
    -- not code anyone steps through, and at -O0 decoding a 4K 10-bit AV1 frame falls behind the sound clock — a Debug
    -- game then drops every frame of its startup movie and shows the last, faded-out one (2026-10-05).
    optimize "Speed"
    -- MSVC refuses /O2 together with the /RTC1 MSBuild adds to every Debug configuration (D8016).
    runtimechecks "Off"

    filter "system:windows"
        systemversion "latest"
        files { root .. "/src/win32/thread.c" }
        includedirs { root .. "/include/compat/msvc" }
        defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:not windows"
        pic "On"
        defines { "_GNU_SOURCE" }

    filter {}

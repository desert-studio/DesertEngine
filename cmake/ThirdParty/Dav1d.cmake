# BuildScripts/ThirdParty/Dav1d.lua: dav1d (AV1 decoder for Engine/Media) as a C11 library without assembly.
# meson's configure step is replaced by the headers written below into build/generated/dav1d (same path premake
# wrote), and each bit-depth template is instantiated twice (8 and 16 bpc) by a generated one-line .c file.
set(_root "${DESERT_ROOT}/ThirdParty/dav1d")
set(_generated "${DESERT_ROOT}/build/generated/dav1d")
if(NOT EXISTS "${_root}/include/dav1d/dav1d.h")
    message(FATAL_ERROR "ThirdParty/dav1d is empty. It is a submodule: run `git submodule update --init ThirdParty/dav1d`.")
endif()
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_root}/meson.build")
file(READ "${_root}/meson.build" _meson)
if(NOT _meson MATCHES "version[ 	
]*:[ 	
]*'([0-9.]+)'")
    message(FATAL_ERROR "Dav1d.cmake: cannot read the version from ${_root}/meson.build")
endif()
desert_write_generated("${_generated}/vcs_version.h"
    "/* written by BuildScripts/ThirdParty/Dav1d.lua */
#define DAV1D_VERSION \"${CMAKE_MATCH_1}\"
")
desert_write_generated("${_generated}/config.h" [==[
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
]==])
set(_templates cdef_apply_tmpl cdef_tmpl fg_apply_tmpl filmgrain_tmpl ipred_prepare_tmpl ipred_tmpl itx_tmpl
               lf_apply_tmpl loopfilter_tmpl looprestoration_tmpl lr_apply_tmpl mc_tmpl recon_tmpl)
set(_instances "")
foreach(name IN LISTS _templates)
    foreach(depth 8 16)
        desert_write_generated("${_generated}/${name}_${depth}bpc.c"
            "/* written by BuildScripts/ThirdParty/Dav1d.lua */
#define BITDEPTH ${depth}
#include \"src/${name}.c\"
")
    endforeach()
endforeach()
# premake listed the instances by glob (*_8bpc.c, *_16bpc.c) over the generated directory.
file(GLOB _instances_8 "${_generated}/*_8bpc.c")
file(GLOB _instances_16 "${_generated}/*_16bpc.c")
list(SORT _instances_8)
list(SORT _instances_16)
set(_src cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c itx_1d.c lf_mask.c lib.c log.c
         mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c thread_task.c warpmv.c wedge.c)
if(DESERT_IS_WINDOWS)
    list(APPEND _src win32/thread.c)
endif()
list(TRANSFORM _src PREPEND "${_root}/src/")
desert_add_library(Dav1d FOLDER ThirdParty SOURCES ${_src} ${_instances_8} ${_instances_16})
set_target_properties(Dav1d PROPERTIES C_STANDARD 11)
target_include_directories(Dav1d PRIVATE "${_generated}" "${_root}" "${_root}/include" "${_root}/include/dav1d")
desert_optimize_speed(Dav1d)
if(DESERT_IS_WINDOWS)
    target_include_directories(Dav1d PRIVATE "${_root}/include/compat/msvc")
    target_compile_definitions(Dav1d PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_target_properties(Dav1d PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(Dav1d PRIVATE _GNU_SOURCE)
endif()

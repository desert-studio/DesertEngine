-- Embree 4 (RenderKit/embree, Apache 2.0; ThirdParty/embree/LICENSE.txt, listed in ThirdParty/THIRD_PARTY_LICENSES.md).
-- Pinned submodule at the v4.4.1 release tag, compiled from source like assimp (Assimp.lua): the upstream CMake is
-- not run, so its configure step is replaced by the headers in BuildScripts/ThirdParty/EmbreeConfig/.
--
-- Why it is here: UE builds mesh distance fields on Embree (MeshUtilities/Private/MeshDistanceFieldUtilities.cpp:
-- rtcPointQuery for the distance, rtcIntersect1 for the sign), and our port does too
-- (Engine/Assets/MeshDistanceFieldBuild.cpp).
--
-- THE OPTIONS (what CMake would be given; EmbreeConfig/ is the configure_file output of exactly these):
--   EMBREE_TASKING_SYSTEM=INTERNAL   no TBB (taskschedulerinternal.cpp)
--   EMBREE_STATIC_LIB=ON             one more static library, linked like the others
--   EMBREE_GEOMETRY_TRIANGLE=ON      every other geometry type OFF (quad, curve, subdiv, user, instance(s), grid, point)
--   EMBREE_RAY_MASK=ON, EMBREE_FILTER_FUNCTION=ON (defaults), EMBREE_RAY_PACKETS=OFF (UE traces rtcIntersect1)
--   ONE ISA, the lowest: SSE2 on x64 (MSVC and clang: /D__SSE__ /D__SSE2__, msvc.cmake FLAGS_SSE2), NEON on ARM64
--   (EMBREE_ISA_NEON: the SSE2 code path through common/simd/arm/sse2neon.h, clang.cmake FLAGS_SSE2). The per-ISA
--   kernel libraries (embree_sse42/avx/avx2/avx512, NEON2X) are not built: no runtime ISA dispatch.
-- The source list is kernels/CMakeLists.txt EMBREE_LIBRARY_FILES (minus embree.rc and the SYCL/subdiv/packet
-- additions) + common/{sys,math,simd,lexers,tasking}, as their CMakeLists.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/embree"
local config = _MAIN_SCRIPT_DIR .. "/BuildScripts/ThirdParty/EmbreeConfig"

if not os.isfile( root .. "/include/embree4/rtcore.h" ) then
    error( "ThirdParty/embree sources are missing. Run `git submodule update --init ThirdParty/embree`." )
end
if not io.readfile( root .. "/CMakeLists.txt" ):find( "SET(EMBREE_VERSION_MINOR 4)", 1, true ) or
   not io.readfile( root .. "/CMakeLists.txt" ):find( "SET(EMBREE_VERSION_PATCH 1)", 1, true ) then
    error( "ThirdParty/embree is not v4.4.1, which BuildScripts/ThirdParty/EmbreeConfig/ was configured for. " ..
           "Check out the pinned submodule, or regenerate EmbreeConfig/ from the new tag's *.h.in." )
end

project "Embree"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "Embree" )
    warnings "Off"
    -- Upstream compiles Debug at -O3 / /Ox too (clang.cmake, msvc.cmake): an -O0 BVH build and traversal would
    -- make every distance-field build in a Debug editor minutes long again.
    optimize "Speed"

    local k = root .. "/kernels/"
    files
    {
        k .. "common/device.cpp", k .. "common/stat.cpp", k .. "common/acceln.cpp", k .. "common/accelset.cpp",
        k .. "common/state.cpp", k .. "common/rtcore.cpp", k .. "common/rtcore_builder.cpp",
        k .. "common/scene.cpp", k .. "common/scene_verify.cpp", k .. "common/alloc.cpp",
        k .. "common/geometry.cpp", k .. "common/scene_user_geometry.cpp", k .. "common/scene_instance.cpp",
        k .. "common/scene_instance_array.cpp", k .. "common/scene_triangle_mesh.cpp",
        k .. "common/scene_quad_mesh.cpp", k .. "common/scene_curves.cpp", k .. "common/scene_line_segments.cpp",
        k .. "common/scene_grid_mesh.cpp", k .. "common/scene_points.cpp", k .. "common/motion_derivative.cpp",
        k .. "subdiv/bezier_curve.cpp", k .. "subdiv/bspline_curve.cpp", k .. "subdiv/catmullrom_curve.cpp",
        k .. "geometry/primitive4.cpp", k .. "geometry/instance_intersector.cpp",
        k .. "geometry/instance_array_intersector.cpp",
        k .. "geometry/curve_intersector_virtual_4v.cpp", k .. "geometry/curve_intersector_virtual_4i.cpp",
        k .. "geometry/curve_intersector_virtual_4i_mb.cpp", k .. "geometry/curve_intersector_virtual_8v.cpp",
        k .. "geometry/curve_intersector_virtual_8i.cpp", k .. "geometry/curve_intersector_virtual_8i_mb.cpp",
        k .. "builders/primrefgen.cpp",
        k .. "bvh/bvh.cpp", k .. "bvh/bvh_statistics.cpp", k .. "bvh/bvh4_factory.cpp", k .. "bvh/bvh8_factory.cpp",
        k .. "bvh/bvh_collider.cpp", k .. "bvh/bvh_rotate.cpp", k .. "bvh/bvh_refit.cpp",
        k .. "bvh/bvh_builder.cpp", k .. "bvh/bvh_builder_hair.cpp", k .. "bvh/bvh_builder_hair_mb.cpp",
        k .. "bvh/bvh_builder_morton.cpp", k .. "bvh/bvh_builder_sah.cpp", k .. "bvh/bvh_builder_sah_spatial.cpp",
        k .. "bvh/bvh_builder_sah_mb.cpp", k .. "bvh/bvh_builder_twolevel.cpp", k .. "bvh/bvh_intersector1_bvh4.cpp",

        root .. "/common/sys/sysinfo.cpp", root .. "/common/sys/alloc.cpp", root .. "/common/sys/filename.cpp",
        root .. "/common/sys/library.cpp", root .. "/common/sys/thread.cpp", root .. "/common/sys/estring.cpp",
        root .. "/common/sys/regression.cpp", root .. "/common/sys/mutex.cpp", root .. "/common/sys/condition.cpp",
        root .. "/common/sys/barrier.cpp",
        root .. "/common/math/constants.cpp",
        root .. "/common/simd/sse.cpp",
        root .. "/common/lexers/stringstream.cpp", root .. "/common/lexers/tokenstream.cpp",
        root .. "/common/tasking/taskschedulerinternal.cpp",
    }

    -- kernels/common/default.h includes "../config.h" and device.cpp "../hash.h": both resolve through
    -- EmbreeConfig/kernels/common (an empty directory kept for that). rtcore_common.h includes "rtcore_config.h".
    includedirs { config .. "/kernels/common", config .. "/include/embree4", root .. "/include", root }

    -- EMBREE_LOWEST_ISA: kernels/CMakeLists.txt gives it to the base `embree` library, the one built here.
    defines { "TASKING_INTERNAL", "EMBREE_STATIC_LIB", "EMBREE_TARGET_SSE2", "EMBREE_LOWEST_ISA", "__SSE__", "__SSE2__" }

    filter "system:windows"
        systemversion "latest"
        -- msvc.cmake: /GS- (no stack protector by default); Debug's /RTC1 cannot go with /O2 (D8016).
        buildoptions { "/GS-" }
        runtimechecks "Off"

    filter "system:not windows"
        pic "On"
        -- clang.cmake: strict aliasing off (the SIMD types are punned), char signed on every target.
        buildoptions { "-fno-strict-aliasing", "-fsigned-char" }

    filter {}

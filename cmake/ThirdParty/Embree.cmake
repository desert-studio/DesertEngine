# BuildScripts/ThirdParty/Embree.lua: Embree 4.4.1 for the mesh distance-field builder -- triangles and ONE ISA
# (SSE2, the lowest), internal tasking, static. The configured headers (config.h, rtcore_config.h, hash.h) are
# checked in under BuildScripts/ThirdParty/EmbreeConfig/, generated once from the tag's *.h.in.
# ThirdParty/embree's own CMakeLists.txt is not used: it builds every ISA and geometry type and needs ISPC/TBB options.
set(_root "${DESERT_ROOT}/ThirdParty/embree")
set(_config "${DESERT_ROOT}/BuildScripts/ThirdParty/EmbreeConfig")
if(NOT EXISTS "${_root}/include/embree4/rtcore.h")
    message(FATAL_ERROR "ThirdParty/embree sources are missing. Run `git submodule update --init ThirdParty/embree`.")
endif()
file(READ "${_root}/CMakeLists.txt" _embree_cmake)
string(FIND "${_embree_cmake}" "SET(EMBREE_VERSION_MINOR 4)" _minor)
string(FIND "${_embree_cmake}" "SET(EMBREE_VERSION_PATCH 1)" _patch)
if(_minor EQUAL -1 OR _patch EQUAL -1)
    message(FATAL_ERROR "ThirdParty/embree is not v4.4.1, which BuildScripts/ThirdParty/EmbreeConfig/ was configured for. "
                        "Check out the pinned submodule, or regenerate EmbreeConfig/ from the new tag's *.h.in.")
endif()
set(_k
    common/device.cpp common/stat.cpp common/acceln.cpp common/accelset.cpp common/state.cpp common/rtcore.cpp
    common/rtcore_builder.cpp common/scene.cpp common/scene_verify.cpp common/alloc.cpp common/geometry.cpp
    common/scene_user_geometry.cpp common/scene_instance.cpp common/scene_instance_array.cpp
    common/scene_triangle_mesh.cpp common/scene_quad_mesh.cpp common/scene_curves.cpp common/scene_line_segments.cpp
    common/scene_grid_mesh.cpp common/scene_points.cpp common/motion_derivative.cpp
    subdiv/bezier_curve.cpp subdiv/bspline_curve.cpp subdiv/catmullrom_curve.cpp
    geometry/primitive4.cpp geometry/instance_intersector.cpp geometry/instance_array_intersector.cpp
    geometry/curve_intersector_virtual_4v.cpp geometry/curve_intersector_virtual_4i.cpp
    geometry/curve_intersector_virtual_4i_mb.cpp geometry/curve_intersector_virtual_8v.cpp
    geometry/curve_intersector_virtual_8i.cpp geometry/curve_intersector_virtual_8i_mb.cpp
    builders/primrefgen.cpp
    bvh/bvh.cpp bvh/bvh_statistics.cpp bvh/bvh4_factory.cpp bvh/bvh8_factory.cpp bvh/bvh_collider.cpp
    bvh/bvh_rotate.cpp bvh/bvh_refit.cpp bvh/bvh_builder.cpp bvh/bvh_builder_hair.cpp bvh/bvh_builder_hair_mb.cpp
    bvh/bvh_builder_morton.cpp bvh/bvh_builder_sah.cpp bvh/bvh_builder_sah_spatial.cpp bvh/bvh_builder_sah_mb.cpp
    bvh/bvh_builder_twolevel.cpp bvh/bvh_intersector1_bvh4.cpp)
list(TRANSFORM _k PREPEND "${_root}/kernels/")
set(_c
    sys/sysinfo.cpp sys/alloc.cpp sys/filename.cpp sys/library.cpp sys/thread.cpp sys/estring.cpp
    sys/regression.cpp sys/mutex.cpp sys/condition.cpp sys/barrier.cpp math/constants.cpp simd/sse.cpp
    lexers/stringstream.cpp lexers/tokenstream.cpp tasking/taskschedulerinternal.cpp)
list(TRANSFORM _c PREPEND "${_root}/common/")
desert_add_library(Embree FOLDER ThirdParty SOURCES ${_k} ${_c})
desert_warnings_off(Embree)  # premake: warnings "Off"
set_target_properties(Embree PROPERTIES CXX_STANDARD 17)
target_include_directories(Embree PRIVATE
    "${_config}/kernels/common" "${_config}/include/embree4" "${_root}/include" "${_root}")
target_compile_definitions(Embree PRIVATE
    TASKING_INTERNAL EMBREE_STATIC_LIB EMBREE_TARGET_SSE2 EMBREE_LOWEST_ISA __SSE__ __SSE2__)
desert_optimize_speed(Embree)
if(DESERT_IS_WINDOWS)
    target_compile_options(Embree PRIVATE /GS-)
    desert_ascii_characterset(Embree)
else()
    set_target_properties(Embree PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(Embree PRIVATE -fno-strict-aliasing -fsigned-char)
endif()

#pragma once

// THE GPU RECORDS OF THE VIRTUAL SHADOW MAP, declared once: Tools/ShaderProtocolGen writes their GLSL twins into
// Generated/Shadows/VirtualShadowMap.glslh (read by Common/VirtualShadowMap.glslh and the "VSM: *" programs) and
// checks the std430 layout against these declarations at compile time.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic::System
{
#define DESERT_VSM_FRAME_GPU_FIELDS( X ) \
    X( glm::mat4, InvViewProjection, "InvViewProjection", glm::mat4( 1.0f ) ) \
    /* xyz camera cm, w = world size of one render pixel per cm of distance */ \
    X( glm::vec4, CameraFootprint, "CameraFootprint", glm::vec4( 0.0f ) ) \
    X( glm::vec4, Lod, "Lod", glm::vec4( 0.0f ) ) /* x LOD bias, y level count, z first level, w frame index */ \
    X( glm::vec4, ToSun, "ToSun", glm::vec4( 0.0f ) ) /* xyz toward the sun, w SourceAngle radians */ \
    /* x rays, y samples per ray, z normal bias texels, w ray length pages */ \
    X( glm::vec4, Smrt, "Smrt", glm::vec4( 0.0f ) ) \
    /* x samples per local ray, y extrapolation max slope, zw 0 (std430 pad) */ \
    X( glm::vec4, SmrtLocal, "SmrtLocal", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( VsmFrameGpu, "VsmFrame", Std430, DESERT_VSM_FRAME_GPU_FIELDS );
#undef DESERT_VSM_FRAME_GPU_FIELDS

#define DESERT_VSM_LEVEL_GPU_FIELDS( X ) \
    /* world -> level clip, orthographic, standard Z */ \
    X( glm::mat4, ViewProjection, "ViewProjection", glm::mat4( 1.0f ) ) \
    /* xy window origin (world page), z absolute level, w 1 = reset now */ \
    X( glm::ivec4, Window, "Window", glm::ivec4( 0 ) ) \
    /* x page world cm, y depth range cm, z radius cm, w texel world cm */ \
    X( glm::vec4, Extent, "Extent", glm::vec4( 0.0f ) ) \
    /* x first page-table entry, y pages per side (0 = unused level), z kVsmLevelKind*, w local light index */ \
    X( glm::uvec4, Pages, "Pages", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( VsmLevelGpu, "VsmLevel", Std430, DESERT_VSM_LEVEL_GPU_FIELDS );
#undef DESERT_VSM_LEVEL_GPU_FIELDS

    // One point / spot light shaded from the map this frame (VsmLocalLight in Common/VirtualShadowMap.glslh).
#define DESERT_VSM_LOCAL_LIGHT_GPU_FIELDS( X ) \
    X( glm::vec4, Position, "Position", glm::vec4( 0.0f ) ) /* xyz cm, w attenuation radius cm */ \
    /* xyz out of the apex, w cos outer (kVsmLocalPointCone = point) */ \
    X( glm::vec4, Spot, "Spot", glm::vec4( 0.0f ) ) \
    /* x level of view 0 mip 0, y view count, z mask layer, w finest mip */ \
    X( glm::uvec4, Views, "Views", glm::uvec4( 0u ) ) \
    /* x near cm, y far cm, z texel world size at unit distance (mip 0), w source cm */ \
    X( glm::vec4, Params, "Params", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( VsmLocalLightGpu, "VsmLocalLight", Std430, DESERT_VSM_LOCAL_LIGHT_GPU_FIELDS );
#undef DESERT_VSM_LOCAL_LIGHT_GPU_FIELDS

#define DESERT_VSM_PAGE_ENTRY_GPU_FIELDS( X ) \
    X( uint32_t, Physical, "Physical", 0 ) \
    X( uint32_t, Flags, "Flags", 0 ) \
    X( int32_t, TagX, "TagX", 0 ) \
    X( int32_t, TagY, "TagY", 0 )
    DESERT_SHADER_STRUCT( VsmPageEntryGpu, "VsmPageEntry", Std430, DESERT_VSM_PAGE_ENTRY_GPU_FIELDS );
#undef DESERT_VSM_PAGE_ENTRY_GPU_FIELDS

    // "VSM: Cull Casters" records (std430; Programs/Shadows/VirtualShadowMapCullCasters.shader).
    // One instance a draw may draw: its world box and which draw it belongs to.
#define DESERT_VSM_CULL_INSTANCE_GPU_FIELDS( X ) \
    X( glm::vec4, Min, "Min", glm::vec4( 0.0f ) ) \
    X( glm::vec4, Max, "Max", glm::vec4( 0.0f ) ) \
    /* x = its draw, y = the instance index its vertex stage fetches, zw 0 */ \
    X( glm::uvec4, Draw, "Draw", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( VsmCullInstanceGpu, "VsmCullInstance", Std430, DESERT_VSM_CULL_INSTANCE_GPU_FIELDS );
#undef DESERT_VSM_CULL_INSTANCE_GPU_FIELDS

    // One draw: its argument records (MeshDrawArguments.hpp: one per drawn submesh, consecutive) and the slots of
    // the instance remap its surviving instances fill (its records' first instance is RemapBase).
#define DESERT_VSM_CULL_DRAW_GPU_FIELDS( X ) \
    X( uint32_t, FirstRecord, "FirstRecord", 0 ) \
    X( uint32_t, Records, "Records", 0 ) \
    X( uint32_t, RemapBase, "RemapBase", 0 ) \
    X( uint32_t, Padding, "Padding", 0 )
    DESERT_SHADER_STRUCT( VsmCullDrawGpu, "VsmCullDraw", Std430, DESERT_VSM_CULL_DRAW_GPU_FIELDS );
#undef DESERT_VSM_CULL_DRAW_GPU_FIELDS

    // The frame's counters and the indirect dispatch of "VSM: Clear Pages" at kVsmClearArgsOffset: the members of
    // the VsmAllocator block of the page programs (Allocate, Gather, Clear Args).
#define DESERT_VSM_ALLOCATOR_GPU_FIELDS( X ) \
    X( uint32_t, EmptyCount, "EmptyCount", 0 ) \
    X( uint32_t, CachedCount, "CachedCount", 0 ) \
    X( uint32_t, AllocationCursor, "AllocationCursor", 0 ) \
    X( uint32_t, DirtyCount, "DirtyCount", 0 ) /* pages cleared and drawn this frame */ \
    X( uint32_t, Overflow, "Overflow", 0 ) /* requests the pool could not hold */ \
    X( uint32_t, StaticRedrawn, "StaticRedrawn", 0 ) /* of DirtyCount, the pages whose static layer is redrawn */ \
    /* of DirtyCount, the pages whose static layer was copied (movers only) */ \
    X( uint32_t, MovableRedrawn, "MovableRedrawn", 0 ) \
    X( uint32_t, Padding, "Padding", 0 ) \
    X( glm::uvec4, ClearArgs, "ClearArgs", glm::uvec4( 0u ) ) /* VkDispatchIndirectCommand of "VSM: Clear Pages" */
    DESERT_SHADER_STRUCT( VsmAllocatorGpu, "VsmAllocatorRecord", Std430, DESERT_VSM_ALLOCATOR_GPU_FIELDS );
#undef DESERT_VSM_ALLOCATOR_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kVirtualShadowMapStructs[] = {
        &kVsmFrameGpuStruct,     &kVsmLevelGpuStruct,     &kVsmLocalLightGpuStruct, &kVsmPageEntryGpuStruct,
        &kVsmCullInstanceGpuStruct, &kVsmCullDrawGpuStruct, &kVsmAllocatorGpuStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kVirtualShadowMapShaderInclude{
        .Path         = "Generated/Shadows/VirtualShadowMap.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMapLayout.hpp",
        .Structs      = kVirtualShadowMapStructs,
    };
} // namespace Desert::Graphic::System

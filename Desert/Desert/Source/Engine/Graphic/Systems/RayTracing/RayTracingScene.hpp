#pragma once

#include <Common/Settings/RenderMethods.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// THE RAY-TRACING SCENE — what a view's TLAS holds this frame (UE FRayTracingScene, its instance list built from
// the scene's primitives). Device-free: this file decides WHICH instances, with which transform, mask and
// GPU-scene row; the Vulkan pass turns the list into VkAccelerationStructureInstanceKHR rows and builds.
//
// ONE GATHER. The candidates are the primitives MeshRenderer already queued and gave a row in the view's
// ObjectMotions buffer (BuildObjectMotions) — UE's GPUScene primitive data: World, PrevWorld, bone offsets. An
// instance's custom index IS that row, so a hit shader and a raster pass read the same primitive data by the same
// number; nothing is gathered twice and no second per-instance table exists.
//
// GATED BY THE METHODS. A frame builds a TLAS only when one of its three resolved methods traces hardware rays
// (Common::Scalability::NeedsRayQuery). ResolveRenderMethods already walked a method the device cannot run down to
// one it can (MethodLimit::Device), so "a resolved method needs ray query" already implies the device has it.
namespace Desert::Graphic::RayTracing
{
    // UE RAY_TRACING_MASK_*: the 8-bit instance mask a ray's cull mask is ANDed with.
    inline constexpr uint8_t kMaskOpaque       = 1u << 0; // every traced primitive (reflections, GI, path tracer)
    inline constexpr uint8_t kMaskShadowCaster = 1u
                                                 << 1; // primitives with CastShadows (RT-SHADOWS traces this bit)

    // VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits.
    inline constexpr uint32_t kMaxGpuSceneRow = ( 1u << 24 ) - 1u;

    // One queued primitive as the TLAS sees it. Filled from MeshRenderer's queues; Geometry is the resident BLAS
    // (null: the mesh has none — not resident yet, or a mesh kind with no BLAS).
    struct RayTracingPrimitive
    {
        const void* Geometry        = nullptr;
        glm::mat4   World           = glm::mat4( 1.0f ); // centimetres, like every world transform
        uint32_t    GpuSceneRow     = 0;                 // the primitive's ObjectMotions row (MotionRow)
        bool        CastShadows     = true;
        uint64_t    HiddenSubmeshes = 0; // bit i = submesh i hidden
        bool        Skinned         = false;
    };

    struct RayTracingInstance
    {
        const void*           Geometry = nullptr;
        std::array<float, 12> Transform{}; // row-major 3x4, VkTransformMatrixKHR
        uint32_t              CustomIndex = 0;
        uint8_t               Mask        = 0;
    };

    // Why a queued primitive is NOT in the TLAS. Counted, never silent: the overlay and the log say how much of
    // the frame the rays cannot see.
    enum class RayTracingSkip : uint8_t
    {
        NoGeometry = 0,  // no resident BLAS for the mesh
        Skinned,         // a deformed mesh needs a per-frame BLAS from skinned positions (a GPU skin cache)
        HiddenSubmeshes, // one BLAS holds every submesh; an instance cannot hide part of it
        RowOutOfRange,   // the GPU-scene row does not fit the 24-bit custom index
        Count
    };

    struct RayTracingInstanceList
    {
        std::vector<RayTracingInstance>                                         Instances;
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )> Skipped{};

        [[nodiscard]] uint32_t SkippedFor( RayTracingSkip reason ) const
        {
            return Skipped[static_cast<std::size_t>( reason )];
        }
    };

    template <typename MethodSet>
    [[nodiscard]] constexpr bool RayTracingSceneRequired( const MethodSet& methods )
    {
        return Common::Scalability::NeedsRayQuery( methods.GlobalIllumination.Effective ) ||
               Common::Scalability::NeedsRayQuery( methods.Reflections.Effective ) ||
               Common::Scalability::NeedsRayQuery( methods.Shadows.Effective );
    }

    [[nodiscard]] inline std::array<float, 12> ToRowMajor3x4( const glm::mat4& world )
    {
        std::array<float, 12> rows{};
        for ( int row = 0; row < 3; ++row )
            for ( int column = 0; column < 4; ++column )
                rows[static_cast<std::size_t>( row * 4 + column )] = world[column][row]; // glm is column-major
        return rows;
    }

    [[nodiscard]] inline RayTracingInstanceList
    BuildRayTracingInstances( std::span<const RayTracingPrimitive> primitives )
    {
        RayTracingInstanceList list;
        list.Instances.reserve( primitives.size() );
        const auto skip = [&list]( RayTracingSkip reason ) { ++list.Skipped[static_cast<std::size_t>( reason )]; };
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.Skinned )
            {
                skip( RayTracingSkip::Skinned );
                continue;
            }
            if ( primitive.Geometry == nullptr )
            {
                skip( RayTracingSkip::NoGeometry );
                continue;
            }
            if ( primitive.HiddenSubmeshes != 0 )
            {
                skip( RayTracingSkip::HiddenSubmeshes );
                continue;
            }
            if ( primitive.GpuSceneRow > kMaxGpuSceneRow )
            {
                skip( RayTracingSkip::RowOutOfRange );
                continue;
            }
            RayTracingInstance& instance = list.Instances.emplace_back();
            instance.Geometry            = primitive.Geometry;
            instance.Transform           = ToRowMajor3x4( primitive.World );
            instance.CustomIndex         = primitive.GpuSceneRow;
            instance.Mask =
                 static_cast<uint8_t>( kMaskOpaque | ( primitive.CastShadows ? kMaskShadowCaster : 0u ) );
        }
        return list;
    }
} // namespace Desert::Graphic::RayTracing

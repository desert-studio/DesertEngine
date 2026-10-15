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

    // Which queue a TLAS instance came from (UE: the primitive's scene proxy kind). Counted per kind so the overlay
    // says what the rays see: a frame whose foliage or landscape is missing from the TLAS shows a zero here.
    enum class RayTracingPrimitiveKind : uint8_t
    {
        Static = 0, // an asset mesh on the lit queue
        Generic,    // a mesh drawn with a data-driven material (shader override, per-slot material, procedural)
        Instanced,  // ONE instance of an ISM / foliage field: every instance is its own GPU-scene row
        Landscape,  // one landscape tile at the view's LOD
        Skinned,    // a deformed mesh, traced through its per-frame BLAS
        Count
    };

    // One TLAS instance as the gather produced it: ONE GPU-scene row (an ObjectMotions row), ONE instance. An ISM
    // instance has a row of its own (the GI card table is keyed per instance, 1:1 with the TLAS instance), so
    // the custom index alone names the instance a ray hit. Geometry is the BLAS the instance traces: the mesh's
    // own, or the (mesh, hidden mask) variant when the row hides some submeshes (RayTracingGeometryVariants);
    // null: the mesh has none (not resident, or a kind with no BLAS). For a skinned primitive Geometry is its
    // per-frame dynamic BLAS (null until one is built for this frame) and DeformedMesh the mesh it skins.
    struct RayTracingPrimitive
    {
        const void*             Geometry        = nullptr;
        glm::mat4               World           = glm::mat4( 1.0f ); // centimetres, like every world transform
        uint32_t                GpuSceneRow     = 0;                 // the primitive's ObjectMotions row (MotionRow)
        bool                    CastShadows     = true;
        uint64_t                HiddenSubmeshes = 0; // bit i = submesh i hidden; Geometry already leaves them out
        RayTracingPrimitiveKind Kind            = RayTracingPrimitiveKind::Static;
        const void*             DeformedMesh    = nullptr; // skinned only: the SkinnedMesh it skins
    };

    // ONE PER-FRAME BLAS BUILD FOR A DEFORMED PRIMITIVE (UE FRayTracingDynamicGeometryUpdateParams): the skin pass
    // writes the primitive's posed positions, then its BLAS is rebuilt from them in the frame of the TLAS that
    // instances it. Primitive indexes the frame's primitive list, the slot whose Geometry the build fills.
    struct RayTracingDynamicGeometryRequest
    {
        uint32_t    Primitive       = 0;
        const void* DeformedMesh    = nullptr;
        uint64_t    HiddenSubmeshes = 0;
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
        Skinned,         // a deformed mesh with no dynamic BLAS built this frame (SelectDynamicGeometry)
        NothingVisible,  // every submesh of the row is hidden: there is no triangle to trace
        RowOutOfRange,   // the GPU-scene row does not fit the 24-bit custom index
        Count
    };

    struct RayTracingInstanceList
    {
        std::vector<RayTracingInstance>                                         Instances;
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )> Skipped{};
        std::array<uint32_t, static_cast<std::size_t>( RayTracingPrimitiveKind::Count )> InstancesByKind{};

        [[nodiscard]] uint32_t SkippedFor( RayTracingSkip reason ) const
        {
            return Skipped[static_cast<std::size_t>( reason )];
        }
        [[nodiscard]] uint32_t InstancesFor( RayTracingPrimitiveKind kind ) const
        {
            return InstancesByKind[static_cast<std::size_t>( kind )];
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

    // The deformed primitives that get a BLAS this frame: skinned, with a mesh, and with a GPU-scene row the TLAS
    // can carry (a build for a primitive the instance list would drop anyway is wasted GPU time). In primitive
    // order, so builds and instances agree on the slot. Hidden submeshes do not exclude: the build leaves them
    // out.
    [[nodiscard]] inline std::vector<RayTracingDynamicGeometryRequest>
    SelectDynamicGeometry( std::span<const RayTracingPrimitive> primitives )
    {
        std::vector<RayTracingDynamicGeometryRequest> requests;
        for ( std::size_t i = 0; i < primitives.size(); ++i )
        {
            const RayTracingPrimitive& primitive = primitives[i];
            if ( primitive.Kind != RayTracingPrimitiveKind::Skinned || primitive.DeformedMesh == nullptr ||
                 primitive.GpuSceneRow > kMaxGpuSceneRow )
                continue;
            RayTracingDynamicGeometryRequest& request = requests.emplace_back();
            request.Primitive                         = static_cast<uint32_t>( i );
            request.DeformedMesh                      = primitive.DeformedMesh;
            request.HiddenSubmeshes                   = primitive.HiddenSubmeshes;
        }
        return requests;
    }

    [[nodiscard]] inline RayTracingInstanceList
    BuildRayTracingInstances( std::span<const RayTracingPrimitive> primitives )
    {
        RayTracingInstanceList list;
        list.Instances.reserve( primitives.size() );
        const auto skip = [&list]( RayTracingSkip reason ) { ++list.Skipped[static_cast<std::size_t>( reason )]; };
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.Kind == RayTracingPrimitiveKind::Skinned && primitive.Geometry == nullptr )
            {
                skip( RayTracingSkip::Skinned );
                continue;
            }
            if ( primitive.Geometry == nullptr )
            {
                skip( RayTracingSkip::NoGeometry );
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
            ++list.InstancesByKind[static_cast<std::size_t>( primitive.Kind )];
        }
        return list;
    }
} // namespace Desert::Graphic::RayTracing

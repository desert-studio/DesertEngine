#pragma once

#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// THE RAY-TRACING SCENE — what a view's TLAS holds this frame (UE FRayTracingScene, its instance list built from
// the scene's primitives). Device-free: this file decides WHICH instances, with which transform, mask and
// GPU-scene row; the Vulkan pass turns the list into VkAccelerationStructureInstanceKHR rows and builds.
//
// ONE GATHER. The candidates are the primitives MeshRenderer already queued and gave a GpuScene instance
// (MeshRenderer::SyncGpuScene) — UE's GPUScene primitive/instance data: World, PrevWorld, bone offsets. An
// instance's custom index IS that GpuScene instance index, so a hit shader and a raster pass read the same primitive data by the same
// number; nothing is gathered twice and no second per-instance table exists.
//
// GATED BY THE METHODS. A frame builds a TLAS only when one of its three resolved methods traces hardware rays
// (Common::Scalability::NeedsRayQuery; Shadows Stochastic by its StochasticVisibility). ResolveRenderMethods already walked a method the device cannot run down to
// one it can (MethodLimit::Device), so "a resolved method needs ray query" already implies the device has it.
namespace Desert
{
    class Mesh;
}
namespace Desert::Graphic
{
    class Material;
    class MaterialInstance;
} // namespace Desert::Graphic

namespace Desert::Graphic::RayTracing
{
    // UE RAY_TRACING_MASK_*: the 8-bit instance mask a ray's cull mask is ANDed with.
    inline constexpr uint8_t kMaskOpaque       = 1u << 0; // every traced primitive (reflections, GI, path tracer)
    inline constexpr uint8_t kMaskShadowCaster = 1u
                                                 << 1; // primitives with CastShadows (RT-SHADOWS traces this bit)
    // Primitives drawn only by BlendMode Translucent / Additive sections (UE RAY_TRACING_MASK_TRANSLUCENT): such a
    // primitive carries THIS bit (plus kMaskTranslucentShadowCaster below) and no other, so the opaque and
    // shadow-caster rays (reflections, GI, RT shadows, the path tracer's opaque light-ray leg) pass it, as the
    // raster's opaque passes and shadow maps do; the path tracer's material rays (cull mask 0xFF) meet it.
    inline constexpr uint8_t kMaskTranslucent = 1u << 2;
    // A translucent primitive with CastShadows (UE PATHTRACER_MASK_SHADOW on a translucent instance): the path
    // tracer's light rays walk THIS bit for transmittance (PathTracer.shader PathTracingTransmittance), as its
    // opaque leg traces kMaskShadowCaster - a primitive with CastShadows off never shadows a path-traced light,
    // exactly as it never enters a shadow map.
    inline constexpr uint8_t kMaskTranslucentShadowCaster = 1u << 3;

    struct RayTracingProgramHit; // RayTracingProgramHit.hpp

    // VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits. Its last value is no row:
    // kNoGpuSceneRow is the custom index of an instance with no GPU-scene row (the GPU-written grass instances,
    // RayTracingGpuInstanceSource). Every reader of the custom index takes it as "no row": the GI tracer's card
    // table (GISurfaceCacheRowForInstance) is never that long, so such a hit reads GI_NO_INSTANCE and is lit in
    // place, as a landscape tile's is.
    inline constexpr uint32_t kNoGpuSceneRow  = ( 1u << 24 ) - 1u;
    inline constexpr uint32_t kMaxGpuSceneRow = kNoGpuSceneRow - 1u;
    // VkAccelerationStructureInstanceKHR::instanceShaderBindingTableRecordOffset is 24 bits too.
    inline constexpr uint32_t kMaxSegmentRecord = ( 1u << 24 ) - 1u;
    // RayTracingSegmentRecord::LandscapeTile of a segment that is not a landscape tile's.
    inline constexpr uint32_t kNoLandscapeTile = 0xFFFFFFFFu;

    // THE MATERIAL OF ONE BLAS SEGMENT (UE: the FRayTracingMeshCommand of a segment, the record its hit group
    // runs, found at the instance's InstanceContributionToHitGroupIndex + the segment's geometry index). The
    // frame's records lie end to end in RayTracingGatherResult::SegmentRecords; a primitive's run starts at its
    // FirstSegmentRecord and holds one record per segment of its BLAS, in segment order. Its TLAS instance carries
    // that start as the instance's SBT record offset, so an inline ray query names the record of a hit as
    //   rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT + rayQueryGetIntersectionGeometryIndexEXT
    // and reads geometry record [that index] (RayTracingSceneMetadata.hpp: record i is built from segment record
    // i, the one table on both sides). The instances of one ISM share their mesh's run.
    struct RayTracingSegmentRecord
    {
        // The section's slot instance (static, ISM, skinned: the instance the raster binds for that section).
        // Null for a generic record (a per-slot data-driven material, bound with no instance) and a landscape
        // tile.
        const MaterialInstance* Instance = nullptr;
        // The material the raster shades the section with: the slot's surface template (SectionSurface), or a
        // generic record's SlotMaterial. Null: a whole-entity shader override (its material is keyed by shader
        // name, MaterialComponent) and a landscape tile, whose surface is its LandscapeTile.
        const Material* SurfaceMaterial = nullptr;
        // The submesh the segment is (a landscape tile: the segment's index in LandscapeRayTracingLayout).
        uint32_t SubmeshIndex = 0;
        // The segment's own Opaque: false = the material is Masked and a hit runs its alpha test (the same test as
        // the material's shadow caster, MeshRendererShadow.cpp).
        bool Opaque = true;
        // A landscape tile's segment: the tile's index in its producer's per-tile lists
        // (LandscapeRayTracingFrame::Materials / RayTracingExternalPrimitives::LandscapeOpacity), which say what
        // the ground is at a hit (the TerrainRayHit hit group) and where its holes are (the alpha test).
        // kNoLandscapeTile for every mesh segment.
        uint32_t LandscapeTile = kNoLandscapeTile;
        // A generic record's own hit group (RayTracingProgramHit.hpp: 3D text's TextRayHit): what the segment's
        // hit runs and its alpha test, the record's for the frame. Null for every other segment.
        const RayTracingProgramHit* ProgramHit = nullptr;

        bool operator==( const RayTracingSegmentRecord& ) const = default;
    };

    // Which queue a TLAS instance came from (UE: the primitive's scene proxy kind). Counted per kind so the
    // overlay says what the rays see: a frame whose foliage or landscape is missing from the TLAS shows a zero
    // here.
    enum class RayTracingPrimitiveKind : uint8_t
    {
        Static = 0, // an asset mesh on the lit queue
        Generic,    // a mesh drawn with a data-driven material (shader override, per-slot material, procedural)
        Instanced,  // ONE instance of an ISM / foliage field: every instance is its own GPU-scene row
        Landscape,  // one landscape tile at the view's LOD
        Skinned,    // a deformed mesh, traced through its per-frame BLAS
        Procedural, // a static/generic row whose mesh has no uploader BLAS (DynamicMesh): built in the graph
        // A section whose instances the GPU wrote this frame (the landscape grass, GpuInstances.hpp): ONE
        // primitive per producer holds the BLAS and the segment run; its TLAS rows are written on the GPU
        // (RayTracingGpuInstanceSource), not by BuildRayTracingInstances. Counted by the rows reserved for it.
        GpuInstanced,
        Count
    };

    // One TLAS instance as the gather produced it: ONE GPU-scene row (an ObjectMotions row), ONE instance. An ISM
    // instance has a row of its own (the GI card table is keyed per instance, 1:1 with the TLAS instance), so
    // the custom index alone names the instance a ray hit. Geometry is the BLAS the instance traces: the mesh's
    // own, or the (mesh, Submeshes) variant when the row hides a submesh or has a masked section
    // (RayTracingGeometryVariants);
    // null: the mesh has none (not resident, or a kind with no BLAS). For a skinned primitive Geometry is its
    // per-frame dynamic BLAS (null until one is built for this frame) and DeformedMesh the mesh it skins.
    struct RayTracingPrimitive
    {
        const void* Geometry    = nullptr;
        glm::mat4   World       = glm::mat4( 1.0f ); // centimetres, like every world transform
        uint32_t    GpuSceneRow = 0;                 // the primitive's GpuScene instance (GpuInstance)
        bool        CastShadows = true;
        // Every section the primitive draws is BlendMode Translucent / Additive: kMaskTranslucent alone.
        bool Translucent = false;
        // Hidden: bit i = submesh i hidden, Geometry already leaves it out. NonOpaque: submesh i's section
        // material is Masked, Geometry's segment is not opaque. Both are the BLAS key (RayTracingSubmeshMasks).
        RayTracingSubmeshMasks Submeshes;
        // This primitive's run in RayTracingGatherResult::SegmentRecords: one record per segment of Geometry.
        uint32_t                FirstSegmentRecord = 0;
        uint32_t                SegmentRecordCount = 0;
        RayTracingPrimitiveKind Kind               = RayTracingPrimitiveKind::Static;
        const void*             DeformedMesh       = nullptr; // skinned only: the SkinnedMesh it skins
        // Skinned only, set with Geometry by RayTracingSkinCache::AddPasses: the skin-cache stream the BLAS was
        // built from this frame (current positions at the buffer's start, kDeformedPositionLayout) - its device
        // address and its registration in the graph being declared, so a reader of the deformed positions (the
        // emissive light list) reads the very bytes the BLAS did, ordered after the skin pass. 0 / invalid: no
        // BLAS this frame.
        uint64_t       DeformedPositionsAddress = 0;
        RDG::BufferRef DeformedPositions;
        // A mesh with no uploader BLAS (Mesh::GetRayTracingGeometry null, e.g. a DynamicMesh): the RT scene's
        // procedural cache builds its (mesh, Submeshes) BLAS in "RayTracingScene.DynamicGeometry" and fills
        // Geometry (RayTracingProceduralGeometry.hpp). Instanced rows keep Kind Instanced.
        const Mesh* ProceduralMesh = nullptr;
        // The mesh asset the primitive draws (a static row's, an ISM instance's), the identity the surface cache
        // keys an instance by with World (SurfaceCacheScene::PlacementKey): the hardware tracer's hit -> cards.
        // Null where the cache holds no such instance (skinned, landscape).
        const void* SourceMesh = nullptr;
    };

    // ONE PER-FRAME BLAS BUILD FOR A DEFORMED PRIMITIVE (UE FRayTracingDynamicGeometryUpdateParams): the skin pass
    // writes the primitive's posed positions, then its BLAS is rebuilt from them in the frame of the TLAS that
    // instances it. Primitive indexes the frame's primitive list, the slot whose Geometry the build fills.
    struct RayTracingDynamicGeometryRequest
    {
        uint32_t               Primitive    = 0;
        const void*            DeformedMesh = nullptr;
        RayTracingSubmeshMasks Submeshes; // the primitive's: hidden sections left out, masked ones non-opaque
    };

    // THE TLAS INSTANCES OF ONE GPU-SCENE ROW (UE: a primitive's ray-tracing instance range). Every row the frame
    // traces has one contiguous range of TLAS instances; a row that traces nothing has InstanceCount 0. A consumer
    // that needs "which instance of the row" reads rayQueryGetIntersectionInstanceIdEXT - FirstInstance (the GPU
    // copy is RayTracingSceneFrame::PrimitiveInstances, indexed by the custom index).
    struct RayTracingPrimitiveInstances
    {
        uint32_t FirstInstance = 0;
        uint32_t InstanceCount = 0;
    };

    struct RayTracingInstance
    {
        const void*           Geometry = nullptr;
        std::array<float, 12> Transform{}; // row-major 3x4, VkTransformMatrixKHR
        uint32_t              CustomIndex = 0;
        uint8_t               Mask        = 0;
        // The instance's SBT record offset: its primitive's FirstSegmentRecord (RayTracingSegmentRecord).
        uint32_t FirstSegmentRecord = 0;
    };

    // Why a queued primitive is NOT in the TLAS. Counted, never silent: the overlay and the log say how much of
    // the frame the rays cannot see.
    enum class RayTracingSkip : uint8_t
    {
        NoGeometry = 0, // no resident BLAS for the mesh
        Skinned, // a deformed mesh whose skin-cache entry was refused this frame (logged, RayTracingSkinCache)
        NothingVisible,          // every submesh of the row is hidden: there is no triangle to trace
        RowOutOfRange,           // the GPU-scene row does not fit the 24-bit custom index
        SegmentRecordOutOfRange, // the first segment record does not fit the 24-bit SBT record offset
        Count
    };

    // Overlay / log names.
    [[nodiscard]] constexpr std::string_view RayTracingPrimitiveKindName( const RayTracingPrimitiveKind kind )
    {
        switch ( kind )
        {
            case RayTracingPrimitiveKind::Static:
                return "Static";
            case RayTracingPrimitiveKind::Generic:
                return "Generic";
            case RayTracingPrimitiveKind::Instanced:
                return "Instanced";
            case RayTracingPrimitiveKind::Landscape:
                return "Landscape";
            case RayTracingPrimitiveKind::Skinned:
                return "Skinned";
            case RayTracingPrimitiveKind::Procedural:
                return "Procedural";
            case RayTracingPrimitiveKind::GpuInstanced:
                return "GPU instanced";
            case RayTracingPrimitiveKind::Count:
                break;
        }
        return "?";
    }
    [[nodiscard]] constexpr std::string_view RayTracingSkipName( const RayTracingSkip reason )
    {
        switch ( reason )
        {
            case RayTracingSkip::NoGeometry:
                return "No geometry";
            case RayTracingSkip::Skinned:
                return "Skin cache refused";
            case RayTracingSkip::NothingVisible:
                return "Nothing visible";
            case RayTracingSkip::RowOutOfRange:
                return "Row out of range";
            case RayTracingSkip::SegmentRecordOutOfRange:
                return "Segment record out of range";
            case RayTracingSkip::Count:
                break;
        }
        return "?";
    }

    struct RayTracingInstanceList
    {
        std::vector<RayTracingInstance> Instances;
        // Indexed by GPU-scene row, up to the highest row the TLAS holds: the instances of each row.
        std::vector<RayTracingPrimitiveInstances>                                        Ranges;
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )>          Skipped{};
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

    // What GatherRayTracingPrimitives (RayTracingGather.hpp) produced this frame.
    // THE TLAS ROWS OF A GPU-INSTANCED PRIMITIVE (UE: the GPU-scene instances whose FRayTracingInstance
    // descriptors a compute pass writes into the instance buffer, RayTracingInstanceBufferUtil): the producer's
    // transform and argument buffers (the very ones the raster's indirect draw reads), the rows reserved for it
    // after the CPU rows (its instance capacity, known on the CPU) and the indexed-indirect record whose count and
    // first instance it traces. "RayTracingScene.GpuInstances" writes one row per reserved slot: an instance the
    // GPU generated, or an inactive row (mask 0, no BLAS) past the drawn count.
    struct RayTracingGpuInstanceSource
    {
        uint32_t Primitive            = 0; // RayTracingGatherResult::Primitives index: BLAS, segment run, shadows
        const GpuInstanceRefs* Refs   = nullptr;
        uint32_t               Rows   = 0; // reserved rows: the producer's instance capacity
        uint32_t               Record = 0; // the indexed-indirect record (submesh) that states count and first
    };

    struct RayTracingGatherResult
    {
        std::vector<RayTracingPrimitive>                                        Primitives;
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )> Skipped{}; // decided by the gather
        std::vector<std::string> Errors; // one line per variant build that failed this frame
        // Every primitive's per-segment material run (RayTracingSegmentRecord), indexed by FirstSegmentRecord.
        std::vector<RayTracingSegmentRecord>     SegmentRecords;
        std::vector<RayTracingGpuInstanceSource> GpuInstanced; // the GpuInstanced primitives' TLAS rows

        void Clear()
        {
            Primitives.clear();
            GpuInstanced.clear();
            Skipped = {};
            Errors.clear();
            SegmentRecords.clear();
        }
    };

    template <typename MethodSet>
    [[nodiscard]] constexpr bool RayTracingSceneRequired( const MethodSet& methods )
    {
        return Common::Scalability::NeedsRayQuery( methods.GlobalIllumination.Effective ) ||
               Common::Scalability::NeedsRayQuery( methods.Reflections.Effective ) ||
               Common::Scalability::NeedsRayQuery( methods.Shadows.Effective ) ||
               // Stochastic traces only when its visibility source is the ray query (Graphic::ShadowsTraceRays).
               ( methods.Shadows.Effective == Common::Scalability::ShadowMethod::Stochastic &&
                 methods.Visibility == Common::Scalability::StochasticVisibility::RayQuery );
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
    // out, and builds a masked section's segments non-opaque.
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
            request.Submeshes                         = primitive.Submeshes;
        }
        return requests;
    }

    // The TLAS instances of @p primitives, in primitive order, the instances of one GPU-scene row kept contiguous
    // (a row's later primitive joins its first one's range), so RayTracingInstanceList::Ranges names every row's
    // instances as one range. Primitives that cannot be traced are counted by reason, never silently dropped.
    [[nodiscard]] inline RayTracingInstanceList
    BuildRayTracingInstances( std::span<const RayTracingPrimitive> primitives )
    {
        RayTracingInstanceList list;
        const auto skip = [&list]( RayTracingSkip reason ) { ++list.Skipped[static_cast<std::size_t>( reason )]; };
        // Pass 1: which primitives enter, and how many instances each row holds.
        std::vector<uint32_t> traced;
        traced.reserve( primitives.size() );
        for ( std::size_t i = 0; i < primitives.size(); ++i )
        {
            const RayTracingPrimitive& primitive = primitives[i];
            // Its rows are the GPU's (RayTracingGpuInstanceSource): none on the CPU, nothing skipped here.
            if ( primitive.Kind == RayTracingPrimitiveKind::GpuInstanced )
                continue;
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
            if ( primitive.FirstSegmentRecord > kMaxSegmentRecord )
            {
                skip( RayTracingSkip::SegmentRecordOutOfRange );
                continue;
            }
            traced.push_back( static_cast<uint32_t>( i ) );
            if ( primitive.GpuSceneRow >= list.Ranges.size() )
                list.Ranges.resize( static_cast<std::size_t>( primitive.GpuSceneRow ) + 1u );
            ++list.Ranges[primitive.GpuSceneRow].InstanceCount;
        }
        // Pass 2: each row's range starts where its first primitive comes in primitive order.
        std::vector<uint32_t> placed( list.Ranges.size(), 0u );
        uint32_t              next = 0;
        for ( const uint32_t index : traced )
        {
            const uint32_t row = primitives[index].GpuSceneRow;
            if ( placed[row] == 0u )
            {
                list.Ranges[row].FirstInstance = next;
                next += list.Ranges[row].InstanceCount;
                placed[row] = 1u;
            }
        }
        // Pass 3: every instance in its row's range.
        std::fill( placed.begin(), placed.end(), 0u );
        list.Instances.resize( traced.size() );
        for ( const uint32_t index : traced )
        {
            const RayTracingPrimitive& primitive = primitives[index];
            const uint32_t             row       = primitive.GpuSceneRow;
            RayTracingInstance&        instance  = list.Instances[list.Ranges[row].FirstInstance + placed[row]++];
            instance.Geometry                    = primitive.Geometry;
            instance.Transform                   = ToRowMajor3x4( primitive.World );
            instance.CustomIndex                 = row;
            instance.FirstSegmentRecord          = primitive.FirstSegmentRecord;
            instance.Mask =
                 primitive.Translucent
                      ? static_cast<uint8_t>( kMaskTranslucent |
                                              ( primitive.CastShadows ? kMaskTranslucentShadowCaster : 0u ) )
                      : static_cast<uint8_t>( kMaskOpaque | ( primitive.CastShadows ? kMaskShadowCaster : 0u ) );
            ++list.InstancesByKind[static_cast<std::size_t>( primitive.Kind )];
        }
        return list;
    }
} // namespace Desert::Graphic::RayTracing

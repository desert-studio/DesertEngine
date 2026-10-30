#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// THE SCENE'S EMISSIVE TRIANGLES AS LIGHTS (UE 5.8 Path Tracer emissive mesh light sampling; NVIDIA RTXDI: the
// emissive triangles of the scene in the one light list its samplers draw from). ONE list for every consumer -
// the path tracer's next event estimation and the surface cache's direct lighting under EITHER GI tracer
// (GI-EMISSIVE-LIGHTS), MegaLights next - built from the SCENE'S MESHES, not from a TLAS: the gather's rows and
// per-segment records (RayTracingGatherResult::Primitives / SegmentRecords - a scene gather when the frame
// traces no TLAS) and each mesh's own geometry. Every row's segment whose material emits becomes an emissive
// segment, every triangle of it an emissive triangle (world positions, area, power), and a power tree over the
// triangles picks one in proportion to its power (EmissiveLightsRenderer.hpp, Common/EmissiveLights.glslh). A
// frame that builds a TLAS adds the hit lookup (TLAS instance, geometry index) -> row over the same list.
//
// ONE SOURCE OF THE POSITIONS: a segment's triangles are read from the vertices the raster draws and the BLAS is
// built from. A static mesh's: its own vertex stream in the geometry pool (MeshGeometry); a deformed
// (skinned) primitive's BLAS reads the skin cache's current positions (RayTracingPrimitive::DeformedPositions,
// kDeformedPositionLayout), so its emissive segments read those - this frame's skinned pose, ordered after the
// skin pass by the graph. The list is therefore TWO parts under one root:
//   * STATIC: the segments whose positions do not move under an unchanged row - kept across frames on the GPU,
//     re-expanded whole only when an emitter is added, removed or re-materialled (StructureHash), and per row when
//     only a row's transform moved (a moving emitter re-expands its own triangles and their path to the root);
//     the CPU list itself is rebuilt only on a regathered frame or a moved material estimate
//     (EmissiveTriangleListCache);
//   * DYNAMIC: the deformed primitives' segments - re-expanded every frame from that frame's skinned positions
//     (their bytes change with no row changing, so no hash could tell); an animated character re-expands its
//     own triangles, never the static list.
// The sampler picks between the two parts by their total powers (the common root), then descends that part's tree.
//
// Two sides, as the rest of the RT scene:
//   * this file, device-free but for the meshes' buffer addresses: the CPU list of emissive segments (transform,
//     the segment's vertex and index streams, its triangle range in its part, the material's emitted radiance)
//     and, on a frame with a TLAS, the lookup from a hit (TLAS instance, geometry index) to its segment, so a ray
//     that meets an emissive triangle names its row;
//   * EmissiveLightsRenderer.hpp: the graph passes that expand the segments into triangles on the GPU (the
//     vertices live in device buffers only) and reduce their powers into the trees.
//
// The material stays the source of the emitted radiance at a point: a sample on a triangle is lit by what the
// segment's hit group returns there (payload Emissive, a textured emission included), and the tree's powers are
// the estimate: the material's emissive colour x intensity (the segment's Radiance) times the emissive map's
// average over the triangle's UV footprint (Build reads the map's mip whose texel covers that footprint, at the
// triangle's UV centroid), times the world area - computed once per triangle when the static part is expanded.
// A segment the estimate calls dark is not in the list; a ray that meets it still sees its emission, with
// the whole weight (its light pdf is 0), so the estimate decides only how well the light is sampled, never
// what the image converges to.
namespace Desert::Graphic::RayTracing
{
    inline constexpr uint32_t kNoEmissiveSegment = 0xFFFFFFFFu;
    // A lookup slot with this bit names a row of the DYNAMIC segments (its low 31 bits), else a static row.
    inline constexpr uint32_t kEmissiveDynamicSegmentBit = 0x80000000u;

    // The surface templates' emission parameters (StandardSurface, StandardSurfaceMasked, FoliageSurface
    // `Properties`; their EvaluateSurface emits SurfaceEmission( texel, EmissiveColor, EmissiveIntensity )): what
    // the renderer's EmissiveRadianceFn reads from a segment's effective row (MaterialBinder::ReadRowParam). A
    // template without them has no estimate: its segments are not in the list, and their emission keeps the whole
    // weight.
    inline constexpr std::string_view kSurfaceEmissiveColorParam     = "EmissiveColor";
    inline constexpr std::string_view kSurfaceEmissiveIntensityParam = "EmissiveIntensity";
    // The emissive map and the UV transform EvaluateSurface applies before sampling it (StandardSurface.shader).
    inline constexpr std::string_view kSurfaceEmissiveTextureParam = "EmissiveTexture";
    inline constexpr std::string_view kSurfaceUvOffsetParam        = "UVOffset";
    inline constexpr std::string_view kSurfaceUvTilingParam        = "UVTiling";
    inline constexpr std::string_view kSurfaceUvRotationParam      = "UVRotation";

    // A segment whose material has no emissive texture (its estimate is the colour x intensity alone).
    inline constexpr uint32_t kNoEmissiveTexture = 0xFFFFFFFFu;

    // std430, 160 B: Common/EmissiveLights.glslh's EmissiveSegment. One scene row's emissive segment.
    struct EmissiveSegmentGpu
    {
        std::array<glm::vec4, 3> ObjectToWorld{}; // the row's world, row-major 3x4 (RayTracingPrimitive::World)
        glm::vec4 Radiance{ 0.0f };     // rgb: the material's emissive colour x intensity; w: its luminance
        glm::vec4 UvTransform{ 0.0f, 0.0f, 1.0f, 1.0f }; // xy: UV offset, zw: UV tiling (KHR_texture_transform)
        uint64_t  VertexAddress    = 0; // the positions the BLAS was built from (mesh stream / skin-cache stream)
        uint64_t  IndexAddress     = 0; // uint32 indices
        uint64_t  AttributeAddress = 0; // the mesh's own vertex stream (Desert::Vertex): the UVs (they do not deform)
        uint32_t  VertexStride     = 0;
        uint32_t  PositionOffset   = 0; // bytes from a vertex's start to its position (RayTracingGeometrySegment)
        uint32_t  FirstVertex      = 0;
        uint32_t  FirstIndex       = 0; // in indices
        uint32_t  FirstTriangle    = 0; // the segment's first row in its part's triangle list
        uint32_t  TriangleCount    = 0;
        uint32_t  GpuSceneRow      = 0; // the row the segment is drawn on (a TLAS hit's custom index)
        uint32_t  SubmeshIndex     = 0; // the submesh the segment is
        uint32_t  AttributeStride  = 0; // bytes between two attribute-stream vertices
        uint32_t  UvOffset         = 0; // bytes from an attribute vertex's start to its UV
        uint32_t  EmissiveTexture  = kNoEmissiveTexture; // the bindless table slot of the emissive map
        uint32_t  EmissiveSampler  = 0;                  // its bindless sampler index
        float     UvRotation       = 0.0f;               // radians (KHR_texture_transform)
        uint32_t  Reserved         = 0;
    };
    static_assert( sizeof( EmissiveSegmentGpu ) == 160 );

    // What a segment emits as its material states it (the surface templates' EvaluateSurface: SurfaceEmission(
    // EmissiveTexture texel at the transformed UV, EmissiveColor, EmissiveIntensity)): the estimate the tree's powers
    // are made of. Radiance zero: the segment does not emit.
    struct EmissiveMaterialEstimate
    {
        glm::vec3 Radiance{ 0.0f }; // colour x intensity
        uint32_t  Texture = kNoEmissiveTexture;
        uint32_t  Sampler = 0;
        glm::vec2 UvOffset{ 0.0f };
        glm::vec2 UvTiling{ 1.0f };
        float     UvRotation = 0.0f;

        bool operator==( const EmissiveMaterialEstimate& ) const = default;
    };

    // std430, 48 B: Common/EmissiveLights.glslh's EmissiveTriangle, written by "EmissiveLights: Build".
    inline constexpr uint64_t kEmissiveTriangleBytes = 48;

    // The material's estimate of a segment (EmissiveMaterialEstimate::Radiance zero: the segment does not emit).
    using EmissiveEstimateFn = std::function<EmissiveMaterialEstimate( const RayTracingSegmentRecord& )>;

    // THE ESTIMATES BY MATERIAL, NOT BY RECORD. A gather's records name few distinct (surface material, instance)
    // pairs: a regather re-keys the records to them (O(records), only on the frames whose gather was redone), and
    // every call evaluates each PAIR once (O(materials): one effective row read each) - an edited emission, or an
    // emissive map that streamed in (its bindless slot moved), is seen the frame it happens with no record walked.
    class EmissiveMaterialTable
    {
    public:
        // True when the records were re-keyed (@p regathered) or an estimate differs from the last call's.
        bool Update( std::span<const RayTracingSegmentRecord> segments, const bool regathered,
                     const EmissiveEstimateFn& estimateOf )
        {
            bool changed = false;
            if ( regathered || m_RecordPair.size() != segments.size() )
            {
                changed = true;
                std::unordered_map<PairKey, uint32_t, PairKeyHash> pairOf;
                m_Representatives.clear();
                m_RecordPair.resize( segments.size() );
                for ( std::size_t r = 0; r < segments.size(); ++r )
                {
                    const RayTracingSegmentRecord& record = segments[r];
                    const PairKey key{ record.SurfaceMaterial, record.Instance,
                                       record.LandscapeTile != kNoLandscapeTile };
                    const auto [found, inserted] =
                         pairOf.try_emplace( key, static_cast<uint32_t>( m_Representatives.size() ) );
                    if ( inserted )
                        m_Representatives.push_back( record );
                    m_RecordPair[r] = found->second;
                }
            }
            m_Scratch.clear();
            m_Scratch.reserve( m_Representatives.size() );
            for ( const RayTracingSegmentRecord& record : m_Representatives )
            {
                EmissiveMaterialEstimate estimate = estimateOf( record );
                estimate.Radiance                 = glm::max( estimate.Radiance, glm::vec3( 0.0f ) );
                m_Scratch.push_back( estimate );
            }
            changed = changed || m_Scratch != m_Estimates;
            std::swap( m_Scratch, m_Estimates );
            return changed;
        }

        // The estimate of segment record @p record of the gather the last Update was given.
        [[nodiscard]] const EmissiveMaterialEstimate& Of( const std::size_t record ) const
        {
            return m_Estimates[m_RecordPair[record]];
        }
        [[nodiscard]] std::size_t GetRecordCount() const
        {
            return m_RecordPair.size();
        }
        [[nodiscard]] std::size_t GetMaterialCount() const
        {
            return m_Estimates.size();
        }

    private:
        struct PairKey
        {
            const Material*         SurfaceMaterial = nullptr;
            const MaterialInstance* Instance        = nullptr;
            bool                    Landscape       = false;
            bool                    operator==( const PairKey& ) const = default;
        };
        struct PairKeyHash
        {
            std::size_t operator()( const PairKey& key ) const
            {
                const std::size_t material = std::hash<const void*>{}( key.SurfaceMaterial );
                const std::size_t instance = std::hash<const void*>{}( key.Instance );
                return ( material * 1099511628211ull ) ^ ( instance + 0x9E3779B97F4A7C15ull + ( material << 6u ) ) ^
                       static_cast<std::size_t>( key.Landscape );
            }
        };

        std::vector<uint32_t>                 m_RecordPair;      // segment record -> its pair
        std::vector<RayTracingSegmentRecord>  m_Representatives; // one record of each pair (what estimateOf reads)
        std::vector<EmissiveMaterialEstimate> m_Estimates;       // per pair
        std::vector<EmissiveMaterialEstimate> m_Scratch;
    };

    struct EmissiveTriangleList
    {
        // Each part's rows: ascending FirstTriangle, end to end, from 0 in that part.
        std::vector<EmissiveSegmentGpu> Segments;        // static
        std::vector<EmissiveSegmentGpu> DynamicSegments; // the deformed primitives', this frame's pose
        // The skin-cache streams the dynamic segments read (one per deformed primitive with an emissive segment),
        // as registered in the graph the primitives were gathered for: the dynamic Build pass reads them.
        std::vector<RDG::BufferRef> DynamicPositions;
        // Indexed by TLAS instance first (InstanceCount entries): the absolute index in Lookup of the instance's
        // per-geometry slots, or kNoEmissiveSegment for an instance with no emissive segment; then the slots,
        // each the row of that (instance, geometry index) - static, or dynamic with kEmissiveDynamicSegmentBit -
        // or kNoEmissiveSegment.
        std::vector<uint32_t> Lookup;
        uint32_t              InstanceCount        = 0;
        uint32_t              TriangleCount        = 0; // static
        uint32_t              DynamicTriangleCount = 0;
        // Emissive segments of deformed primitives left out this frame: their skin cache wrote no positions (no
        // ray-traced scene this frame). Counted, so the caller says it (AddFrameEmissiveLights).
        uint32_t UnplacedDeformedSegments = 0;
        // Per gather primitive: its static rows [x, x + y) in Segments (y = 0: none - not emitting, deformed, or a
        // grass row), what a moved primitive rewrites (EmissiveTriangleListCache::Update with moved primitives).
        std::vector<glm::uvec2> PrimitiveRows;
        // FNV-1a of the static rows WITHOUT their transforms (ObjectToWorld zeroed): which triangles the static
        // part holds, from which streams, at which leaves, with which emission - not where they stand. The
        // renderer re-expands the whole static part on a change; otherwise only the rows whose transform moved
        // (EmissiveLightsRenderer: "EmissiveLights: Move", then Build and Reduce over their leaves' runs).
        uint64_t StructureHash = 0;
        // FNV-1a of Lookup: the renderer re-uploads the lookup alone on a change (the TLAS order moved).
        uint64_t LookupHash = 0;
        // FNV-1a of everything: the static rows with their transforms, the lookup, the dynamic rows - what this
        // frame's whole list is (the path tracer restarts on a change).
        uint64_t Hash = 0;
    };

    // FNV-1a over @p data from @p hash: the list's hashes (EmissiveTriangleList).
    [[nodiscard]] inline uint64_t EmissiveFnv( uint64_t hash, const std::span<const std::byte> data )
    {
        for ( const std::byte b : data )
            hash = ( hash ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
        return hash;
    }
    inline constexpr uint64_t kEmissiveFnvBasis = 14695981039346656037ull;

    [[nodiscard]] inline float EmissiveLuminance( const glm::vec3& c )
    {
        return glm::dot( c, glm::vec3( 0.2126f, 0.7152f, 0.0722f ) );
    }

    // PURE but for the meshes' device addresses (VertexBuffer / IndexBuffer::GetDeviceAddress). THE LIST IS BUILT
    // FROM THE SCENE'S MESHES, NOT FROM A TLAS: @p primitives / @p segments are a gather's rows and segment records
    // (GatherRayTracingPrimitives - the TLAS gather when this frame builds one, a SCENE gather otherwise; the same
    // rows, transforms, sections and materials either way), and each emissive record's triangles are read from its
    // mesh's own geometry (MeshGeometry: the pool span the raster draws and the BLAS is built from) over the
    // segment rule every BLAS uses (DescribeMeshGeometry with the row's masks: record FirstSegmentRecord + g is
    // segment g). A deformed primitive's records read its skin-cache positions (RayTracingPrimitive::
    // DeformedPositions) - this frame's pose - and its UVs from the mesh's own stream (UVs do not deform).
    // @p materials is the estimate of every record (EmissiveMaterialTable::Update over the same @p segments).
    //
    // @p instances are this frame's TLAS rows in TLAS order (RayTracingInstanceList::Instances), EMPTY when the
    // frame builds no TLAS: they only make the hit lookup (a ray that meets an emissive triangle names its row by
    // TLAS instance + geometry index; instance i is the gather row CustomIndex, its record FirstSegmentRecord + g),
    // so they must come from the same gather as @p primitives.
    //
    // Errors, never a skipped row: a record past the table, a material table of another gather, an emissive mesh
    // with no device address (a device without bufferDeviceAddress - every device with hardware ray tracing has
    // it), a mesh whose segments disagree with its record run, an emitting TLAS segment the list holds no row of.
    // Left out and COUNTED (EmissiveTriangleList::UnplacedDeformedSegments): a deformed primitive's emissive
    // segment on a frame whose skin cache wrote no positions for it (no ray-traced scene this frame; the software
    // tracer's scene holds no deformed mesh either). The landscape grass rows (GpuInstanced: no CPU transform per
    // instance) are not read.
    [[nodiscard]] inline Common::ResultStr<EmissiveTriangleList> BuildEmissiveTriangleList(
         std::span<const RayTracingPrimitive> primitives, std::span<const RayTracingSegmentRecord> segments,
         std::span<const RayTracingInstance> instances, const EmissiveMaterialTable& materials )
    {
        using Result = EmissiveTriangleList;
        if ( materials.GetRecordCount() != segments.size() )
            return Common::MakeFormattedError<Result>( "EmissiveLights: the material table holds {} records and the "
                                                       "gather {}",
                                                       materials.GetRecordCount(), segments.size() );
        Result list;
        list.InstanceCount = static_cast<uint32_t>( instances.size() );
        list.PrimitiveRows.assign( primitives.size(), glm::uvec2( 0u ) );
        const auto radiance = [&materials]( const std::size_t record ) -> const glm::vec3&
        { return materials.Of( record ).Radiance; };

        // (GPU-scene row, segment record) -> its row in its part (dynamic with kEmissiveDynamicSegmentBit).
        std::unordered_map<uint64_t, uint32_t> slotOf;
        const auto key = []( const uint32_t row, const uint32_t record )
        { return ( static_cast<uint64_t>( row ) << 32u ) | record; };

        // The records of the rows walked below (every row but the grass rows): a TLAS segment of any other record
        // that emits is a light the list must hold.
        std::vector<bool> walked( segments.size(), false );
        uint64_t          staticTriangles  = 0;
        uint64_t          dynamicTriangles = 0;
        for ( std::size_t p = 0; p < primitives.size(); ++p )
        {
            const RayTracingPrimitive& primitive = primitives[p];
            if ( primitive.Kind == RayTracingPrimitiveKind::GpuInstanced || primitive.SegmentRecordCount == 0u )
                continue;
            if ( static_cast<std::size_t>( primitive.FirstSegmentRecord ) + primitive.SegmentRecordCount >
                 segments.size() )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: primitive {} (row {}) names segment records {}..{} of {}", p,
                     primitive.GpuSceneRow, primitive.FirstSegmentRecord,
                     primitive.FirstSegmentRecord + primitive.SegmentRecordCount, segments.size() );
            bool emits = false;
            for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
            {
                walked[primitive.FirstSegmentRecord + g] = true;
                emits = emits || EmissiveLuminance( radiance( primitive.FirstSegmentRecord + g ) ) > 0.0f;
            }
            if ( !emits )
                continue;

            const bool deformed = primitive.DeformedMesh != nullptr;
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the gather stores these exact types
            const Mesh* mesh = deformed ? static_cast<const Mesh*>( static_cast<const SkinnedMesh*>( primitive.DeformedMesh ) )
                                        : static_cast<const Mesh*>( primitive.SourceMesh );
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
            if ( mesh == nullptr )
                return Common::MakeFormattedError<Result>( "EmissiveLights: primitive {} (row {}) emits and names "
                                                           "no mesh",
                                                           p, primitive.GpuSceneRow );
            if ( deformed && ( primitive.DeformedPositionsAddress == 0u || !primitive.DeformedPositions.IsValid() ) )
            {
                for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
                    if ( EmissiveLuminance( radiance( primitive.FirstSegmentRecord + g ) ) > 0.0f )
                        ++list.UnplacedDeformedSegments;
                continue;
            }
            const MeshGeometry& geometry      = mesh->GetGeometry();
            const uint64_t      indexBase     = geometry.Indices ? geometry.Indices->GetDeviceAddress() : 0u;
            const uint64_t      attributeBase = geometry.Vertices ? geometry.Vertices->GetDeviceAddress() : 0u;
            const uint64_t      vertexBase    = deformed ? primitive.DeformedPositionsAddress : attributeBase;
            if ( indexBase == 0u || vertexBase == 0u || attributeBase == 0u )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: the emissive mesh of row {} has no device address (not resident, or the "
                     "device has no bufferDeviceAddress)",
                     primitive.GpuSceneRow );
            // The mesh's own spans (indices stay mesh-relative, GeometryPool.hpp); the skin cache's stream starts
            // at the mesh's vertex 0. The UVs are the mesh's own stream (Desert::Vertex) for both kinds, read
            // through the same indices (MeshAttributeLayout).
            const uint64_t indexAddress     = indexBase + geometry.IndexByteOffset;
            const uint64_t attributeAddress = attributeBase + geometry.VertexByteOffset;
            const uint64_t vertexAddress    = deformed ? vertexBase : attributeAddress;
            const std::vector<RayTracingGeometrySegment> described =
                 DescribeMeshGeometry( mesh->GetSubmeshes(), primitive.Submeshes,
                                       deformed ? kDeformedPositionLayout : kStaticMeshVertexLayout );
            if ( described.size() != primitive.SegmentRecordCount )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: row {} has {} segment records and its mesh {} segments", primitive.GpuSceneRow,
                     primitive.SegmentRecordCount, described.size() );
            if ( deformed )
                list.DynamicPositions.push_back( primitive.DeformedPositions );
            else
                list.PrimitiveRows[p].x = static_cast<uint32_t>( list.Segments.size() );

            for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
            {
                const uint32_t                   record   = primitive.FirstSegmentRecord + g;
                const EmissiveMaterialEstimate&  estimate = materials.Of( record );
                const RayTracingGeometrySegment& segment  = described[g];
                if ( EmissiveLuminance( estimate.Radiance ) <= 0.0f || segment.PrimitiveCount == 0u )
                    continue;
                std::vector<EmissiveSegmentGpu>& rows      = deformed ? list.DynamicSegments : list.Segments;
                uint64_t&                        triangles = deformed ? dynamicTriangles : staticTriangles;
                if ( rows.size() >= kEmissiveDynamicSegmentBit )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: {} emissive segments exceed the 31-bit segment index", rows.size() );
                const auto rowIndex = static_cast<uint32_t>( rows.size() );
                slotOf[key( primitive.GpuSceneRow, record )] =
                     deformed ? ( rowIndex | kEmissiveDynamicSegmentBit ) : rowIndex;

                EmissiveSegmentGpu& row = rows.emplace_back();
                // The row's world as a row-major 3x4 (glm is column-major: row r of column c is World[c][r]).
                for ( glm::length_t r = 0; r < 3; ++r )
                    row.ObjectToWorld[static_cast<std::size_t>( r )] =
                         glm::vec4( primitive.World[0][r], primitive.World[1][r], primitive.World[2][r],
                                    primitive.World[3][r] );
                row.Radiance         = glm::vec4( estimate.Radiance, EmissiveLuminance( estimate.Radiance ) );
                row.UvTransform      = glm::vec4( estimate.UvOffset, estimate.UvTiling );
                row.VertexAddress    = vertexAddress;
                row.IndexAddress     = indexAddress;
                row.AttributeAddress = attributeAddress;
                row.VertexStride     = segment.VertexStride;
                row.PositionOffset   = segment.PositionOffset;
                row.FirstVertex      = segment.FirstVertex;
                row.FirstIndex       = segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
                row.FirstTriangle    = static_cast<uint32_t>( triangles );
                row.TriangleCount    = segment.PrimitiveCount;
                row.GpuSceneRow      = primitive.GpuSceneRow;
                row.SubmeshIndex     = segment.SubmeshIndex;
                row.AttributeStride  = static_cast<uint32_t>( sizeof( Vertex ) );
                row.UvOffset         = static_cast<uint32_t>( offsetof( Vertex, TexCoord ) );
                row.EmissiveTexture  = estimate.Texture;
                row.EmissiveSampler  = estimate.Sampler;
                row.UvRotation       = estimate.UvRotation;
                triangles += segment.PrimitiveCount;
                if ( !deformed )
                    ++list.PrimitiveRows[p].y;
                // The triangle index and the trees' leaves are 32-bit on the GPU, and the two parts share one
                // index space (the dynamic triangles follow the static ones).
                if ( staticTriangles + dynamicTriangles > 0x7FFFFFFFull )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: {} emissive triangles exceed the 31-bit triangle index",
                         staticTriangles + dynamicTriangles );
            }
        }
        list.TriangleCount        = static_cast<uint32_t>( staticTriangles );
        list.DynamicTriangleCount = static_cast<uint32_t>( dynamicTriangles );

        // The hit lookup over this frame's TLAS (none without one): instance i's slots, one per geometry index g,
        // name the list row of (its GPU-scene row, its record FirstSegmentRecord + g).
        list.Lookup.assign( instances.size(), kNoEmissiveSegment );
        for ( std::size_t i = 0; i < instances.size(); ++i )
        {
            const RayTracingInstance& instance = instances[i];
            if ( instance.Geometry == nullptr )
                continue;
            const RayTracingGeometryLayout layout =
                 static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMetadataLayout();
            for ( uint32_t g = 0; g < layout.Segments.size(); ++g )
            {
                const uint32_t record = instance.FirstSegmentRecord + g;
                if ( record >= segments.size() )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: TLAS instance {} (row {}) names segment record {} of {}", i,
                         instance.CustomIndex, record, segments.size() );
                const auto found = slotOf.find( key( instance.CustomIndex, record ) );
                if ( found == slotOf.end() )
                {
                    // Not emitting, or a grass row (GpuInstanced: not walked). A TLAS segment that emits while
                    // the list holds no row of it would be lit with no light pdf: refused.
                    if ( walked[record] && EmissiveLuminance( radiance( record ) ) > 0.0f &&
                         layout.Segments[g].PrimitiveCount > 0u )
                        return Common::MakeFormattedError<Result>( "EmissiveLights: TLAS instance {} (row {}) "
                                                                   "segment {} emits and the list holds no row "
                                                                   "of it",
                                                                   i, instance.CustomIndex, g );
                    continue;
                }
                if ( list.Lookup[i] == kNoEmissiveSegment )
                {
                    list.Lookup[i] = static_cast<uint32_t>( list.Lookup.size() );
                    list.Lookup.resize( list.Lookup.size() + layout.Segments.size(), kNoEmissiveSegment );
                }
                list.Lookup[list.Lookup[i] + g] = found->second;
            }
        }

        // The structure: every static row with its transform zeroed.
        uint64_t structure = kEmissiveFnvBasis;
        for ( EmissiveSegmentGpu row : list.Segments )
        {
            row.ObjectToWorld = {};
            structure         = EmissiveFnv( structure, std::as_bytes( std::span<const EmissiveSegmentGpu>( &row, 1 ) ) );
        }
        list.StructureHash = structure;
        list.LookupHash = EmissiveFnv( kEmissiveFnvBasis, std::as_bytes( std::span<const uint32_t>( list.Lookup ) ) );
        uint64_t whole =
             EmissiveFnv( kEmissiveFnvBasis, std::as_bytes( std::span<const EmissiveSegmentGpu>( list.Segments ) ) );
        whole     = EmissiveFnv( whole, std::as_bytes( std::span<const uint32_t>( list.Lookup ) ) );
        list.Hash = EmissiveFnv( whole, std::as_bytes( std::span<const EmissiveSegmentGpu>( list.DynamicSegments ) ) );
        return Common::MakeSuccess( std::move( list ) );
    }

    // THE LIST ACROSS FRAMES (SceneRenderer::AddFrameEmissiveLights). Rebuilt on the CPU only when what it is made
    // of can have changed: the gather it reads was redone (@p regathered: a frame with a TLAS gathers every frame,
    // a scene gather runs only when the GpuScene stream carried a structure change) or a material's estimate moved
    // (EmissiveMaterialTable). A still scene costs the materials' estimates, no primitive walk. The renderer then
    // compares what it keeps on the GPU with the list (StructureHash, then each static row's transform), so a
    // moving emitter re-expands its own triangles only and a static non-emitter never touches the list.
    class EmissiveTriangleListCache
    {
    public:
        [[nodiscard]] Common::BoolResultStr Update( std::span<const RayTracingPrimitive>     primitives,
                                                    std::span<const RayTracingSegmentRecord> segments,
                                                    std::span<const RayTracingInstance> instances, const bool regathered,
                                                    std::span<const uint32_t> moved, const EmissiveEstimateFn& estimateOf )
        {
            const bool materials = m_Materials.Update( segments, regathered, estimateOf );
            if ( m_Valid && !regathered && !materials )
                return Move( primitives, moved );
            m_Valid   = false;
            auto list = BuildEmissiveTriangleList( primitives, segments, instances, m_Materials );
            if ( !list )
                return Common::MakeError( list.GetError() );
            m_List  = std::move( list.GetValue() );
            m_Valid = true;
            ++m_Builds;
            return BOOLSUCCESS;
        }

        // The list of the last successful Update.
        [[nodiscard]] const EmissiveTriangleList& GetList() const
        {
            return m_List;
        }
        // How many times Update rebuilt the CPU list (a still scene: once).
        [[nodiscard]] uint64_t GetBuildCount() const
        {
            return m_Builds;
        }
        // How many times Update moved rows of the kept list in place (a moving emitter: once per frame it moves).
        [[nodiscard]] uint64_t GetMoveCount() const
        {
            return m_Moves;
        }

    private:
        // The kept list, same structure: the static rows of each moved primitive take its World (a row-major 3x4),
        // and Hash (what the whole list is) takes the rewritten rows. StructureHash and the lookup do not move.
        // Walks the moved primitives' rows only.
        [[nodiscard]] Common::BoolResultStr Move( std::span<const RayTracingPrimitive> primitives,
                                                  std::span<const uint32_t>            moved )
        {
            bool rewrote = false;
            for ( const uint32_t p : moved )
            {
                if ( p >= primitives.size() || p >= m_List.PrimitiveRows.size() )
                    return Common::MakeFormattedError<bool>( "EmissiveLights: moved primitive {} of a gather of {} "
                                                             "(the list holds {})",
                                                             p, primitives.size(), m_List.PrimitiveRows.size() );
                const glm::uvec2 rows  = m_List.PrimitiveRows[p];
                const glm::mat4& world = primitives[p].World;
                for ( uint32_t row = rows.x; row < rows.x + rows.y; ++row )
                {
                    EmissiveSegmentGpu& segment = m_List.Segments[row];
                    for ( glm::length_t r = 0; r < 3; ++r )
                        segment.ObjectToWorld[static_cast<std::size_t>( r )] =
                             glm::vec4( world[0][r], world[1][r], world[2][r], world[3][r] );
                    m_List.Hash =
                         EmissiveFnv( m_List.Hash, std::as_bytes( std::span<const EmissiveSegmentGpu>( &segment, 1 ) ) );
                    rewrote = true;
                }
            }
            if ( rewrote )
                ++m_Moves;
            return BOOLSUCCESS;
        }

        EmissiveMaterialTable m_Materials;
        EmissiveTriangleList  m_List;
        bool                  m_Valid  = false;
        uint64_t              m_Builds = 0;
        uint64_t              m_Moves  = 0;
    };

    // One run of tree nodes a Build or Reduce dispatch writes: its threads [ThreadPrefix, ThreadPrefix + Count)
    // write nodes [First, First + Count) of their level. std430 16 B: Common/EmissiveLights.glslh's
    // EmissiveNodeRun.
    struct EmissiveNodeRunGpu
    {
        uint32_t First        = 0;
        uint32_t Count        = 0;
        uint32_t ThreadPrefix = 0;
        uint32_t Reserved     = 0;
    };
    static_assert( sizeof( EmissiveNodeRunGpu ) == 16 );

    // One tree level's runs in EmissiveTreeRuns::Runs.
    struct EmissiveLevelRuns
    {
        uint32_t FirstRun = 0;
        uint32_t RunCount = 0;
        uint32_t Threads  = 0; // the runs' node total: the dispatch's thread count
    };

    struct EmissiveTreeRuns
    {
        std::vector<EmissiveNodeRunGpu> Runs;   // level 0's runs (the leaves Build writes), then level 1's, ...
        std::vector<EmissiveLevelRuns>  Levels; // one per tree level
    };

    // PURE. The nodes each level of a power tree with @p levels levels rewrites when the leaves in @p leafRuns
    // changed ([x, y) half-open, any order, may overlap or touch): level 0 the merged runs, level k the parents of
    // level k - 1's runs, merged - the path from every changed leaf to the root and nothing else. The whole tree
    // is the one run [0, leaves).
    [[nodiscard]] inline EmissiveTreeRuns EmissiveDirtyRuns( std::vector<glm::uvec2> leafRuns, const uint32_t levels )
    {
        EmissiveTreeRuns out;
        std::sort( leafRuns.begin(), leafRuns.end(),
                   []( const glm::uvec2& a, const glm::uvec2& b ) { return a.x < b.x; } );
        std::vector<glm::uvec2> runs;
        const auto              merge = [&runs]( const glm::uvec2 run )
        {
            if ( run.y <= run.x )
                return;
            if ( !runs.empty() && run.x <= runs.back().y )
                runs.back().y = std::max( runs.back().y, run.y );
            else
                runs.push_back( run );
        };
        for ( const glm::uvec2& run : leafRuns )
            merge( run );
        for ( uint32_t level = 0; level < levels; ++level )
        {
            if ( level > 0 )
            {
                const std::vector<glm::uvec2> below = std::move( runs );
                runs.clear();
                for ( const glm::uvec2& run : below ) // still ascending: parents of ascending runs ascend
                    merge( glm::uvec2( run.x >> 1u, ( ( run.y - 1u ) >> 1u ) + 1u ) );
            }
            EmissiveLevelRuns lv;
            lv.FirstRun = static_cast<uint32_t>( out.Runs.size() );
            lv.RunCount = static_cast<uint32_t>( runs.size() );
            for ( const glm::uvec2& run : runs )
            {
                out.Runs.push_back( { .First = run.x, .Count = run.y - run.x, .ThreadPrefix = lv.Threads } );
                lv.Threads += run.y - run.x;
            }
            out.Levels.push_back( lv );
        }
        return out;
    }
} // namespace Desert::Graphic::RayTracing

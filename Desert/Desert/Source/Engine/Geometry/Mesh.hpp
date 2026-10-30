#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/GeometryPool.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include "Errors/MeshError.hpp"
#include "MeshTypes.hpp"
#include <Engine/Geometry/DisplacementTessellation.hpp>
#include <Engine/Geometry/LODSelection.hpp>

#include <Common/Core/ResultWithCodes.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDisplacementBake.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace Desert::Graphic::RayTracing
{
    class RayTracingGeometryResource;
}
namespace Desert::Graphic::VirtualGeometry
{
    class VirtualMeshRegistration;
}

namespace Desert
{
    // A MESH'S RENDER IDENTITY, NEVER REUSED (UE: a render resource's own id, not its address). A cache keyed by
    // the Mesh* (the shadow caches' static-caster signatures) took a freed mesh and a new one built at the same
    // address for the same mesh: after Undo cleared a sculpt the reloaded asset landed where the last stroke's
    // mesh had been, the signature matched, and its shadow pages kept the stroke's depth. Every object draws a
    // fresh value (a copy too: it is another resource), and a content change draws another.
    class MeshRenderRevision
    {
    public:
        MeshRenderRevision() : m_Value( Next() )
        {
        }
        MeshRenderRevision( const MeshRenderRevision& /*other*/ ) : m_Value( Next() )
        {
        }
        MeshRenderRevision& operator=( const MeshRenderRevision& other )
        {
            if ( this != &other )
                m_Value = Next();
            return *this;
        }
        ~MeshRenderRevision() = default;

        void Bump()
        {
            m_Value = Next();
        }
        [[nodiscard]] uint64_t Value() const
        {
            return m_Value;
        }

    private:
        static uint64_t Next()
        {
            static std::atomic<uint64_t> next{ 1 };
            return next.fetch_add( 1, std::memory_order_relaxed );
        }
        uint64_t m_Value;
    };

    class Mesh
    {
    public:
        virtual ~Mesh() = default;

        // Common interface
        [[nodiscard]] virtual MeshType GetType() const = 0;
        [[nodiscard]] bool             IsSkinned() const
        {
            return GetType() == MeshType::Skinned;
        }

        // The mesh's rows in the VirtualMeshTable, asked of EVERY mesh: MeshType::Static names a cooked StaticMesh
        // AND a DynamicMesh (a primitive, an editable mesh, a fracture / spline piece), so a reader of the static
        // queue holds a Mesh* and cannot cast its way to the answer - a DynamicMesh read as a StaticMesh reads past
        // its end (the thumbnail scene's material sphere, FPS-MEASURE-5). Null here: only a StaticMesh whose
        // cooked DAGs were registered (StaticMesh::Invalidate) has rows.
        [[nodiscard]] virtual const Graphic::VirtualGeometry::VirtualMeshRegistration* GetVirtualMesh() const
        {
            return nullptr;
        }

        [[nodiscard]] virtual const std::vector<Submesh>& GetSubmeshes() const
        {
            return m_Submeshes;
        }

        // THE SCREEN SIZE AT WHICH EACH LEVEL OF THIS MESH BEGINS (UE FStaticMeshRenderData::ScreenSize): one
        // entry per level the mesh draws, resolved from its asset's authored values (MeshService, at build) or
        // the automatic rule. Every LOD pick of this mesh - renderer, ISM instances, shadows, Details - walks it.
        [[nodiscard]] const std::vector<float>& GetLODScreenSizes() const
        {
            return m_LODScreenSizes;
        }
        [[nodiscard]] const std::optional<std::vector<float>>& GetAuthoredLODScreenSizes() const
        {
            return m_AuthoredLODScreenSizes;
        }
        // The asset's per-LOD values (nullopt = computed); re-resolved against the levels this mesh has.
        void SetAuthoredLODScreenSizes( std::optional<std::vector<float>> authored )
        {
            m_AuthoredLODScreenSizes = std::move( authored );
            ResolveLODScreenSizes();
        }
        // THE MESH'S GPU GEOMETRY (Graphic/GeometryPool.hpp): where its vertices, streams (binding 1) and indices
        // live - the GeometryPool arena of its vertex layout for every mesh built from CPU data, its own buffers
        // for a twin a compute pass writes. Not resident (IsResident false) until the renderer uploaded it
        // (EnsureGpuResources / Invalidate). A pooled mesh always has streams (default values written when it
        // authored none, MeshGeometry::AuthoredStreams); an unpooled one without streams binds the renderer's
        // shared default buffer.
        [[nodiscard]] const Graphic::MeshGeometry& GetGeometry() const
        {
            return m_Geometry;
        }

        // THE MESH'S BOTTOM-LEVEL ACCELERATION STRUCTURE (UE FRayTracingGeometry on the render data), built from
        // the same vertex and index buffers by the uploader where the device traces rays; null everywhere else
        // (no RT device, a skinned mesh, not uploaded yet). It lives and dies with the mesh, so it is released on
        // the frame-retire rule the mesh's buffers follow (MeshService::RetireEvicted drops the last reference).
        [[nodiscard]] const std::shared_ptr<Graphic::RayTracing::RayTracingGeometryResource>&
        GetRayTracingGeometry() const
        {
            return m_RayTracingGeometry;
        }
        void SetRayTracingGeometry( std::shared_ptr<Graphic::RayTracing::RayTracingGeometryResource> geometry )
        {
            m_RayTracingGeometry = std::move( geometry );
        }

        // THE MESH'S DISPLACEMENT TOPOLOGY (Geometry/DisplacementTessellation.hpp): welded LOD0, clusters, built at
        // cook from the render data and attached by the loader when a section's material declares a Height and the
        // asset's displacement mode is Runtime; null otherwise. DisplacementCache tessellates from it - the CPU
        // vertex data is not kept by the mesh, so the topology is the displaced path's only source.
        // The topology arrives on a worker (Assets::DisplacementTopologySlot); null until it lands.
        [[nodiscard]] std::shared_ptr<const Geometry::DisplacementTopology> GetDisplacementTopology() const
        {
            return m_DisplacementTopology ? m_DisplacementTopology->Get() : nullptr;
        }
        void SetDisplacementTopology( std::shared_ptr<const Assets::DisplacementTopologySlot> slot )
        {
            m_DisplacementTopology = std::move( slot );
        }
        // THE BAKE-LEVEL DISPLACED SURFACE (Assets/MeshDisplacementBake.hpp), any displacement mode: the Baked
        // mode draws it (StaticMesh::GetBakedDisplaced), collision reads it in both modes. Null until it lands.
        [[nodiscard]] std::shared_ptr<const Assets::DisplacedMeshData> GetDisplacedMeshData() const
        {
            return m_DisplacedMesh ? m_DisplacedMesh->Get() : nullptr;
        }
        void SetDisplacedMesh( std::shared_ptr<const Assets::DisplacedMeshSlot> slot )
        {
            m_DisplacedMesh = std::move( slot );
        }
        // Baked mode: a displaced surface with no runtime topology (Runtime has the topology, and the surface when
        // the asset's own materials assign a Height).
        [[nodiscard]] bool IsBakedDisplaced() const
        {
            return m_DisplacedMesh != nullptr && m_DisplacementTopology == nullptr;
        }
        // The mesh is displaced (its import settings chose a displacement mode): its draws over displaced geometry
        // take the displaced material slots (MaterialSlotBinding::Displaced), even before the geometry lands.
        [[nodiscard]] bool IsDisplaced() const
        {
            return m_DisplacementTopology != nullptr || m_DisplacedMesh != nullptr;
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() = 0;

        // The mesh's signed distance field (Assets::MeshDistanceFieldData) once it has landed in the slot shared
        // with the asset it was loaded with (the field is derived asynchronously,
        // Assets::EnqueueMeshDistanceField); null before that and for a mesh that has none (skinned, procedural).
        // What the GPU scene's distance-field atlas uploads (Graphic/DistanceField), keyed by the field's pointer.
        [[nodiscard]] std::shared_ptr<const Assets::MeshDistanceFieldData> GetDistanceField() const
        {
            return m_DistanceField ? m_DistanceField->Get() : nullptr;
        }
        // Whether the mesh is mostly two-sided (foliage; Assets::MeshDistanceFieldSlot::MostlyTwoSided).
        [[nodiscard]] bool IsDistanceFieldMostlyTwoSided() const
        {
            return m_DistanceField && m_DistanceField->MostlyTwoSided();
        }
        void SetDistanceField( std::shared_ptr<const Assets::MeshDistanceFieldSlot> slot )
        {
            m_DistanceField = std::move( slot );
        }
        // The mesh's surface-cache cards (Assets::MeshCardsBuildData), derived beside its distance field; null
        // before they land and for a mesh that has none.
        [[nodiscard]] std::shared_ptr<const Assets::MeshCardsBuildData> GetCards() const
        {
            return m_Cards ? m_Cards->Get() : nullptr;
        }
        void SetCards( std::shared_ptr<const Assets::MeshCardsSlot> slot )
        {
            m_Cards = std::move( slot );
        }

        // THE GPU BUFFERS ARE THE RENDERER'S COPY OF THE CPU GEOMETRY, BUILT WHERE A RENDERER EXISTS. A mesh
        // whose data was set or edited without a device (headless, server, tests, a game-thread write) only
        // marks its GPU side stale; the renderer calls this when the mesh is submitted for drawing (the
        // pattern of UE's render proxy / InitResources). One upload per change: the flag drops whether or
        // not the upload succeeds, so a failure is reported once, not once per frame.
        [[nodiscard]] Common::BoolResultWithCodes<MeshError> EnsureGpuResources()
        {
            if ( !m_GpuStale )
                return Common::MakeSuccessWithCodes<bool, MeshError>( true );
            m_GpuStale = false;
            return Invalidate();
        }
        [[nodiscard]] bool IsGpuStale() const
        {
            return m_GpuStale;
        }
        // What a renderer cache keys this mesh's content by (MeshRenderRevision): never the address.
        [[nodiscard]] uint64_t GetRenderRevision() const
        {
            return m_RenderRevision.Value();
        }

    protected:
        // The CPU data changed and the GPU buffers no longer hold it (or were never made).
        void MarkGpuStale()
        {
            m_GpuStale = true;
            m_RenderRevision.Bump();
            // A BLAS built from the old positions would let rays hit geometry the raster no longer draws.
            m_RayTracingGeometry.reset();
        }
        bool               m_GpuStale = false;
        MeshRenderRevision m_RenderRevision;

        // Called by every subclass that (re)builds its LOD chain: the resolved sizes follow the level count.
        void ResolveLODScreenSizes()
        {
            m_LODScreenSizes = Geometry::ResolveLODScreenSizes( Geometry::MaxAvailableLOD( m_Submeshes ) + 1,
                                                                m_AuthoredLODScreenSizes );
        }
        std::optional<std::vector<float>> m_AuthoredLODScreenSizes;
        std::vector<float>                m_LODScreenSizes;

        std::shared_ptr<Graphic::RayTracing::RayTracingGeometryResource> m_RayTracingGeometry;

        // THE CPU DATA WAITING FOR ITS UPLOAD (StaticMesh / SkinnedMesh: the constructor's arrays, kept until the
        // renderer uploads them into the pool and then released, as the per-mesh buffers released their host
        // copy). Vertices are raw bytes of the arena's vertex type.
        // The pool's index layout: three uint32 per triangle, mesh-relative.
        [[nodiscard]] static std::vector<uint32_t> FlattenIndices( const std::vector<Index>& triangles )
        {
            std::vector<uint32_t> flat;
            flat.reserve( triangles.size() * 3 );
            for ( const Index& t : triangles )
            {
                flat.push_back( t.V1 );
                flat.push_back( t.V2 );
                flat.push_back( t.V3 );
            }
            return flat;
        }
        template <typename TVertex>
        [[nodiscard]] static std::vector<std::byte> VertexBytes( const std::vector<TVertex>& vertices )
        {
            const auto* first = reinterpret_cast<const std::byte*>( vertices.data() );
            return std::vector<std::byte>( first, first + vertices.size() * sizeof( TVertex ) );
        }
        // A twin over buffers it does not own in the pool (GPU-written by a displacement / morph pass).
        void SetUnpooledGeometry( Graphic::MeshGeometry geometry )
        {
            m_PoolAllocation.reset();
            m_Staged   = false;
            m_Geometry = std::move( geometry );
        }

        void StageGeometry( Graphic::GeometryArena arena, std::vector<std::byte> vertices,
                            std::vector<MeshVertexStreams> streams, std::vector<uint32_t> indices,
                            std::vector<uint32_t> clusterWords = {} )
        {
            m_StagedArena        = arena;
            m_StagedVertices     = std::move( vertices );
            m_StagedStreams      = std::move( streams );
            m_StagedIndices      = std::move( indices );
            m_StagedClusterWords = std::move( clusterWords );
            m_Staged             = true;
        }
        // Uploads the staged data into a new pool allocation (the previous one released on the frame rule) and
        // drops the staged copy once it landed. Nothing staged: the geometry already on the GPU stands (a twin
        // built over GPU-written buffers, or a mesh whose staged copy already landed); refused when there is none.
        [[nodiscard]] Common::BoolResultWithCodes<MeshError> UploadStagedGeometry()
        {
            if ( !m_Staged )
            {
                if ( m_Geometry.IsResident() )
                    return Common::MakeSuccessWithCodes<bool, MeshError>( true );
                return Common::MakeErrorWithCodes<bool, MeshError>(
                     { MeshError::GpuUploadFailed },
                     "mesh geometry uploaded again after its host copy was released" );
            }
            const bool authored = !m_StagedStreams.empty();
            auto       uploaded = Graphic::GeometryPool::Get().RT_Upload(
                 m_StagedArena, m_StagedVertices, m_StagedStreams, m_StagedIndices, m_StagedClusterWords );
            if ( !uploaded.IsSuccess() )
                return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                    uploaded.GetError() );
            AdoptPoolAllocation( uploaded.ExtractValue(), authored );
            m_Staged = false;
            std::vector<std::byte>().swap( m_StagedVertices );
            std::vector<MeshVertexStreams>().swap( m_StagedStreams );
            std::vector<uint32_t>().swap( m_StagedIndices );
            std::vector<uint32_t>().swap( m_StagedClusterWords );
            return Common::MakeSuccessWithCodes<bool, MeshError>( true );
        }
        // The mesh now draws from @p allocation (shared by a copy of the mesh, as its buffers were).
        void AdoptPoolAllocation( Graphic::GeometryPoolAllocation allocation, const bool authoredStreams )
        {
            m_PoolAllocation = std::make_shared<Graphic::GeometryPoolAllocation>( std::move( allocation ) );
            m_Geometry       = Graphic::GeometryPool::Get().Describe( *m_PoolAllocation, authoredStreams );
        }
        // Drops the GPU geometry (the pool span released on the frame rule).
        void ReleaseGeometry()
        {
            m_PoolAllocation.reset();
            m_Geometry = {};
        }

        Graphic::MeshGeometry                            m_Geometry;
        std::shared_ptr<Graphic::GeometryPoolAllocation> m_PoolAllocation;
        bool                                             m_Staged      = false;
        Graphic::GeometryArena                           m_StagedArena = Graphic::GeometryArena::Static;
        std::vector<std::byte>                           m_StagedVertices;
        std::vector<MeshVertexStreams>                   m_StagedStreams;
        std::vector<uint32_t>                            m_StagedIndices;
        std::vector<uint32_t>                            m_StagedClusterWords; // VG DAG words after the indices
        std::vector<std::vector<TriangleCache>> m_TriangleCache;
        std::vector<Submesh>                    m_Submeshes;
        std::shared_ptr<const Assets::MeshDistanceFieldSlot> m_DistanceField;
        std::shared_ptr<const Assets::MeshCardsSlot>         m_Cards;
        std::shared_ptr<const Assets::DisplacementTopologySlot> m_DisplacementTopology;

    protected:
        std::shared_ptr<const Assets::DisplacedMeshSlot> m_DisplacedMesh;
        [[nodiscard]] const std::shared_ptr<const Assets::MeshDistanceFieldSlot>& DistanceFieldSlot() const
        {
            return m_DistanceField;
        }
        [[nodiscard]] const std::shared_ptr<const Assets::MeshCardsSlot>& CardsSlot() const
        {
            return m_Cards;
        }
    };
} // namespace Desert

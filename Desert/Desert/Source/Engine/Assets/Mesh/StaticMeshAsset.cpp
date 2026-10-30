#include "StaticMeshAsset.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>

#include <format>
#include <functional>
#include <algorithm>
#include <optional>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

namespace Desert::Assets
{
    StaticMeshAsset::StaticMeshAsset( const Common::Filepath& filepath ) : MeshAsset( filepath, GetTypeID() )
    {
        // The path-derived handle this type used to compute for itself (twice — here and again in Load)
        // now comes from AssetBase, which derives it the same way for every asset type. See the comment on
        // that constructor.
    }

    Common::BoolResultStr StaticMeshAsset::LoadFromFile()
    {
        // A STATIC MESH ON DISK IS ITS SOURCE ASSET (MeshSourceAsset, AF4d); what this class draws is the render
        // form DERIVED from it (UE: FStaticMeshRenderData from the source models via the DDC). The DDC answers
        // with the MeshBinary container, built on a miss where a builder is registered (the editor). A file
        // that is not a mesh asset is refused there by name; there is no second reader for the old cook.
        const auto raw = LoadMeshPlatformDataKeyed( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );

        // ONE READER, ONE FORM: the render data is the binary container, decoded in one place that is
        // testable without a filesystem.
        const auto dataReflected =
             Serialization::ReadMeshAssetData( raw.GetValue().Bytes, m_Metadata.Filepath.string() );
        if ( !dataReflected )
        {
            return Common::MakeError( dataReflected.GetError() );
        }

        const auto& data = dataReflected.GetValue();

        // Safety check
        if ( data.IsSkinned )
        {
            return Common::MakeError( "StaticMeshAsset cannot load skinned mesh data." );
        }

        // AN EMPTY SUCCESSFUL LOAD IS A SILENT WRONG ANSWER (DC 1.4). A submesh is the unit this engine
        // draws — MeshECSSystem iterates GetSubmeshes() and nothing else — so a mesh that parses to zero of
        // them is a file with vertices nobody will ever issue a draw for. Reported here, with the numbers,
        // rather than after: refusing BEFORE the members are cleared leaves the asset exactly as it was and
        // IsReadyForUse false, so the caller sees "not loaded" instead of "loaded, and empty", which is the
        // distinction this whole task is about.
        if ( data.Submeshes.empty() )
        {
            return Common::MakeFormattedError(
                 "'{}' parsed but carries ZERO submeshes ({} vertices, {} triangles), so nothing in it can "
                 "be drawn. The cooked file is incomplete — re-cook it (Assets > Rebuild Cooked Assets).",
                 m_Metadata.Filepath.string(), data.StaticVertices.size(), data.Indices.size() );
        }

        m_Vertices.clear();
        m_Indices.clear();
        m_Submeshes.clear();
        // WAS MISSING, AND IT IS THE ONE ACCUMULATING MEMBER IN THIS FUNCTION. The loop below
        // `emplace_back`s one handle per submesh; without this clear a SECOND Load — which is exactly what
        // eviction plus EnsureLoaded produces, and what a hot reload would produce the day meshes get one
        // — leaves m_Submeshes.size() == N while m_MaterialAssetHandles.size() == 2N, with the STALE
        // handles occupying indices [0,N). `GetMaterialHandles()` would then answer with a vector twice
        // as long as the submesh list, whose first N entries are the material assignment from BEFORE the
        // reload — so a reload whose whole purpose is to pick up an edited file would silently keep the
        // old bindings, and the vector would grow by N on every cycle.
        m_MaterialAssetHandles.clear();
        m_MaterialGuids.clear();

        m_Submeshes.reserve( data.Submeshes.size() );
        m_MaterialAssetHandles.reserve( data.Submeshes.size() );

        for ( const auto& s : data.Submeshes )
        {
            Submesh submesh;
            submesh.Name         = s.Name;
            submesh.VertexOffset = s.VertexOffset;
            submesh.VertexCount  = s.VertexCount;
            submesh.IndexOffset  = s.IndexOffset;
            submesh.IndexCount   = s.IndexCount;
            submesh.Transform    = s.Transform;
            submesh.BoundingBox  = s.BoundingBox;

            m_MaterialGuids.push_back( s.MaterialGuid );
            m_MaterialAssetHandles.emplace_back(
                 s.MaterialGuid.IsNull() ? Common::UUID::Null()
                                         : Common::UUID( static_cast<uint64_t>(
                                                Common::Content::HandleForGuid( s.MaterialGuid ) ) ) );
            m_Submeshes.emplace_back( std::move( submesh ) );
        }
        {
            std::scoped_lock lock( m_CpuGeometryMutex );
            m_CpuGeometryReleased = false;
            m_CpuAccessRequested  = false;
            FillCpuGeometry( data );
        }

        // Every submesh or none carries a DAG (the cook builds one per submesh when the import flag is on).
        m_ClusterDags.clear();
        if ( std::any_of( data.Submeshes.begin(), data.Submeshes.end(),
                          []( const auto& s ) { return !s.ClusterDag.Clusters.empty(); } ) )
        {
            m_ClusterDags.reserve( data.Submeshes.size() );
            for ( const auto& s : data.Submeshes )
                m_ClusterDags.push_back( s.ClusterDag );
        }

        m_MorphTargets.clear();
        m_MorphTargets.reserve( data.MorphTargets.size() );
        for ( const auto& mt : data.MorphTargets )
            m_MorphTargets.push_back( MorphTarget{ mt.Name, mt.DeltaPositions, mt.DeltaNormals } );

        // The displaced products are keyed on this render data (the cards and the undisplaced field on their own
        // triangles, MeshDistanceFieldInputContentKey); ResolveDependencies derives them once the materials load.
        m_RenderDataKey  = raw.GetValue().Key;
        m_ImportSettings = raw.GetValue().Settings;
        m_DistanceField.reset();
        m_Cards.reset();
        m_DependenciesResolved = false;

        m_IsReadyForUse = true;
        return BOOLSUCCESS;
    }

    void StaticMeshAsset::FillCpuGeometry( const Serialization::MeshAssetData& data )
    {
        m_Vertices.clear();
        m_Indices.clear();
        m_Vertices.reserve( data.StaticVertices.size() );
        m_Indices.reserve( data.Indices.size() );
        for ( const auto& v : data.StaticVertices )
        {
            Vertex vertex;
            vertex.Position  = v.Position;
            vertex.Normal    = v.Normal;
            vertex.Tangent   = v.Tangent;
            vertex.Bitangent = v.Bitangent;
            vertex.TexCoord  = v.TexCoord;
            m_Vertices.emplace_back( std::move( vertex ) );
        }
        for ( const auto& i : data.Indices )
            m_Indices.push_back( { i.V1, i.V2, i.V3 } );

        // Baked LOD chain (if the asset was cooked with LODs): the simplified triangle sets StaticMesh appends
        // instead of re-simplifying at load. Empty -> generated at load. CPU render data like the arrays above.
        for ( size_t s = 0; s < m_Submeshes.size() && s < data.Submeshes.size(); ++s )
        {
            auto& baked = m_Submeshes[s].BakedLODs;
            baked.clear();
            baked.reserve( data.Submeshes[s].LODs.size() );
            for ( const auto& lvl : data.Submeshes[s].LODs )
            {
                std::vector<Index> tris;
                tris.reserve( lvl.size() );
                for ( const auto& t : lvl )
                    tris.push_back( { t.V1, t.V2, t.V3 } );
                baked.push_back( std::move( tris ) );
            }
        }
        m_VertexStreams = PackMeshVertexStreams( data.Colors, data.UV1, m_Vertices.size() );
    }

    void StaticMeshAsset::ReleaseCpuGeometry()
    {
        std::scoped_lock lock( m_CpuGeometryMutex );
        if ( !m_IsReadyForUse || m_CpuGeometryReleased || m_CpuAccessRequested )
            return;
        // UE bAllowCPUAccess=false: once the GPU buffers hold the render data, the CPU arrays are released (UE
        // FStaticMeshVertexBuffer::bNeedsCPUAccess=false discards them after InitRHI). Swapped with empties, not
        // clear(): clear keeps the capacity, and the capacity is the memory.
        std::vector<Vertex>().swap( m_Vertices );
        std::vector<Index>().swap( m_Indices );
        std::vector<MeshVertexStreams>().swap( m_VertexStreams );
        for ( auto& submesh : m_Submeshes )
            std::vector<std::vector<Index>>().swap( submesh.BakedLODs );
        m_CpuGeometryReleased = true;
    }

    void StaticMeshAsset::EnsureCpuGeometry( const bool keep ) const
    {
        std::scoped_lock lock( m_CpuGeometryMutex );
        if ( !m_CpuGeometryReleased )
            return;
        // A CPU reader after the GPU upload: read the same render data back (the key the load decoded) and keep it
        // for the rest of this load. A different key is a different mesh behind the same arrays' ranges - refused.
        auto* self                  = const_cast<StaticMeshAsset*>( this );
        self->m_CpuAccessRequested  = self->m_CpuAccessRequested || keep;
        self->m_CpuGeometryReleased = false;
        const auto raw              = LoadMeshPlatformDataKeyed( m_Metadata.Filepath );
        if ( !raw )
        {
            LOG_ERROR( "StaticMeshAsset '{}': CPU access after the GPU upload could not re-read the render data "
                       "({}); CPU readers (collision, picking) see no triangles",
                       m_Metadata.Filepath.string(), raw.GetError() );
            return;
        }
        if ( raw.GetValue().Key != m_RenderDataKey )
        {
            LOG_ERROR( "StaticMeshAsset '{}': CPU access after the GPU upload found render data {:016x}, the GPU "
                       "holds {:016x} - the source changed without a reload; CPU readers see no triangles",
                       m_Metadata.Filepath.string(), raw.GetValue().Key, m_RenderDataKey );
            return;
        }
        const auto data = Serialization::ReadMeshAssetData( raw.GetValue().Bytes, m_Metadata.Filepath.string() );
        if ( !data )
        {
            LOG_ERROR(
                 "StaticMeshAsset '{}': CPU access after the GPU upload could not decode the render data ({})",
                 m_Metadata.Filepath.string(), data.GetError() );
            return;
        }
        self->FillCpuGeometry( data.GetValue() );
        if ( keep )
            LOG_INFO(
                 "StaticMeshAsset '{}': CPU geometry read back for a CPU reader ({} vertices); kept for this load",
                 m_Metadata.Filepath.string(), m_Vertices.size() );
    }

    Common::BoolResultStr StaticMeshAsset::Unload()
    {
        m_Vertices.clear();
        m_Indices.clear();
        m_Submeshes.clear();
        m_MorphTargets.clear();
        // The fifth vector, which this body cleared and Load did not. See the clear in Load for what the
        // two omissions together did to a reload; either one alone was harmless, which is why neither was
        // noticed.
        m_MaterialAssetHandles.clear();
        m_DistanceField.reset();
        m_Cards.reset();
        m_DisplacementTopology.reset();
        m_DisplacedMesh.reset();
        m_MaterialGuids.clear();
        m_MaterialGuids.shrink_to_fit();
        m_DependenciesResolved = false;
        {
            std::scoped_lock lock( m_CpuGeometryMutex );
            m_CpuGeometryReleased = false;
            m_CpuAccessRequested  = false;
        }

        m_Vertices.shrink_to_fit();
        m_Indices.shrink_to_fit();
        m_Submeshes.shrink_to_fit();
        m_MorphTargets.shrink_to_fit();
        m_VertexStreams.clear();
        m_VertexStreams.shrink_to_fit();
        m_ClusterDags.clear();
        m_ClusterDags.shrink_to_fit();
        m_MaterialAssetHandles.shrink_to_fit();

        // Same reason as SkinnedMeshAsset::Unload: the flag is what EnsureLoaded asks before deciding to
        // parse, so an emptied asset that still reports "ready" is one nobody will ever reload.
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }

    namespace
    {
        // The two inputs of a static mesh's distance field and cards: every submesh's material sidedness (a slot
        // naming no material draws the engine's Default Surface, MeshECSSystem::DefaultInstance, a one-sided
        // template) and the LOD0 triangles.
        struct FieldInputs
        {
            std::vector<uint8_t>        SubmeshTwoSided;
            MeshDistanceFieldBuildInput Input;
        };

        Common::ResultStr<FieldInputs>
        ResolveFieldInputs( AssetManager& manager, const std::vector<Common::Content::AssetGuid>& materials,
                            const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                            const std::vector<Submesh>& submeshes )
        {
            std::vector<uint8_t> twoSided( materials.size(), 0 );
            for ( size_t i = 0; i < materials.size(); ++i )
            {
                if ( materials[i].IsNull() )
                    continue;
                const auto sided = ResolveMaterialTwoSided( manager, materials[i] );
                if ( !sided )
                    return Common::MakeFormattedError<FieldInputs>(
                         "submesh {} has no distance field material sidedness ({})", i, sided.GetError() );
                twoSided[i] = sided.GetValue() ? 1 : 0;
            }
            auto input = MeshDistanceFieldInputOf( vertices, indices, submeshes, twoSided );
            if ( !input )
                return Common::MakeError<FieldInputs>( std::string( input.GetError() ) );
            return Common::MakeSuccess( FieldInputs{ std::move( twoSided ), input.ExtractValue() } );
        }

        using LazyFieldInput = std::function<Common::ResultStr<MeshDistanceFieldBuildInput>()>;

        // A lazy input asked at most once: the cards, their coverage and the field each ask it on a miss of their
        // own bucket, and a displaced mesh's surface (Runtime: its densest level) is built once per derivation,
        // not once per bucket.
        LazyFieldInput OnceInput( LazyFieldInput input )
        {
            struct Once
            {
                bool                                       Asked = false;
                std::optional<MeshDistanceFieldBuildInput> Value;
                std::string                                Error;
            };
            auto once = std::make_shared<Once>();
            return [input = std::move( input ), once]() -> Common::ResultStr<MeshDistanceFieldBuildInput>
            {
                if ( !once->Asked )
                {
                    once->Asked = true;
                    auto in     = input();
                    if ( in.IsSuccess() )
                        once->Value = in.ExtractValue();
                    else
                        once->Error = std::string( in.GetError() );
                }
                if ( once->Value.has_value() )
                    return Common::MakeSuccess( *once->Value );
                return Common::MakeError<MeshDistanceFieldBuildInput>( once->Error );
            };
        }

        // THE ONE DERIVATION of a static mesh's cards and distance field (UE FCardRepresentationAsyncQueue +
        // FDistanceFieldAsyncQueue): ResolveDependencies' queued job and the package cook
        // (DeriveDistanceFieldAndCardsNow) both run it, so a packaged game asks for exactly the DDC keys the cook
        // wrote. The cards first (a DDC hit or a build from the same triangles), handed to @p cardsOutcome; a
        // cancelled card build is the job's cancel (DfRun: not a failure, nothing logged) and is not handed over.
        // @p key is the surface's key and @p input its triangles, asked only on a miss (OnceInput).
        Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>> DeriveCardsThenField(
             const std::filesystem::path& path, uint64_t key, const std::vector<uint8_t>& sided,
             const LazyFieldInput& input, const MeshDistanceFieldCancel& cancel, bool& built,
             const std::function<void( Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>> )>&
                  cardsOutcome )
        {
            const LazyFieldInput once  = OnceInput( input );
            auto                 cards = LoadMeshCards( path, key, sided, once, cancel );
            if ( !cards.IsSuccess() && cards.GetError().find( kMeshCardsCancelled ) != std::string::npos )
                return Common::MakeError<std::shared_ptr<const MeshDistanceFieldData>>(
                     std::string( kMeshDistanceFieldCancelled ) );
            cardsOutcome( std::move( cards ) );
            return LoadMeshDistanceField( path, key, sided, once, &built, cancel );
        }

        // DISPLACEMENT (CONTRACT-DISPLACE §1g, §9), resolved where the manager can load the materials: every
        // section's height as its material resolves it on the CPU. nullopt = nothing to displace (mode None, or
        // Baked with no section Height). Runtime always derives the topology: the component's override
        // material may displace a mesh whose own materials do not (TopologyOnly).
        // - Runtime: the topology lands in a slot the scene's DisplacementCache dices from (§9.1).
        // - Both modes: the bake-level surface (MeshDisplacementBake.hpp) - drawn in Baked mode, collision in
        // both.
        // - SDF and cards follow the displaced surface: Baked = the baked mesh, Runtime = the maximum level
        // (§9.12),
        //   each keyed from that surface's key, so software tracing sees the relief the rasteriser draws.
        struct DisplacementPlan
        {
            MeshDisplacementMode                     Mode = MeshDisplacementMode::None;
            std::shared_ptr<DisplacedMeshBuildInput> Input; // the job sets its Topology once loaded
            uint64_t                                 DisplacedKey = 0;
            uint64_t                                 SurfaceKey   = 0; // what SDF and cards key from
            // Runtime with no Height on the asset's own materials: only the topology is derived. The component
            // that draws the mesh may still displace it through its override material (resolved per frame by
            // MeshRenderer::ApplyDisplacement, as UE's component material drives Nanite displacement); the
            // asset's surface, collision, cards and field stay the undisplaced render data.
            bool TopologyOnly = false;
        };

        Common::ResultStr<std::optional<DisplacementPlan>>
        ResolveDisplacementPlan( AssetManager& manager, const MeshImportSettings& settings, uint64_t renderDataKey,
                                 const std::vector<Common::Content::AssetGuid>& materials,
                                 const std::vector<Vertex>&                     vertices,
                                 const std::vector<MeshVertexStreams>& streams, const std::vector<Index>& indices,
                                 const std::vector<Submesh>& submeshes )
        {
            using Plan                      = std::optional<DisplacementPlan>;
            const MeshDisplacementMode mode = settings.Displacement;
            if ( mode == MeshDisplacementMode::None )
                return Common::MakeSuccess( Plan{} );
            std::vector<std::optional<SubmeshHeightInput>> heights( submeshes.size() );
            bool                                           anyHeight = false;
            for ( size_t i = 0; i < submeshes.size() && i < materials.size(); ++i )
            {
                if ( materials[i].IsNull() )
                    continue;
                auto height = ResolveSubmeshHeight( manager, materials[i] );
                if ( !height.IsSuccess() )
                    return Common::MakeFormattedError<Plan>( "submesh {} has no resolvable height ({})", i,
                                                             height.GetError() );
                heights[i] = height.ExtractValue();
                anyHeight  = anyHeight || heights[i].has_value();
            }
            if ( !anyHeight && mode != MeshDisplacementMode::Runtime )
                return Common::MakeSuccess( Plan{} );
            DisplacementPlan plan;
            plan.TopologyOnly = !anyHeight;
            plan.Mode  = mode;
            plan.Input = std::make_shared<DisplacedMeshBuildInput>(
                 DisplacedMeshBuildInput{ vertices, streams, indices, submeshes, nullptr, heights,
                                          settings.DisplacementBake.EdgeLengthCm, true, settings.LodChain } );
            plan.DisplacedKey =
                 DisplacedMeshDerivedDataKey( renderDataKey, plan.Input->EdgeLengthCm, true, heights );
            plan.SurfaceKey = mode == MeshDisplacementMode::Baked
                                   ? plan.DisplacedKey
                                   : DisplacedMeshDerivedDataKey( renderDataKey, std::nullopt, false, heights );
            return Common::MakeSuccess( Plan{ std::move( plan ) } );
        }

        // The surface the cards and the field are derived from: the render data (@p base), or the displaced
        // surface of @p plan. Loads (or builds where the builders are registered) the topology and the bake-level
        // surface first, publishing them to the slots when given (the job; the cook passes none). A product that
        // cannot be had is reported in @p error (the job logs it, the cook returns it) and the cards and field
        // fall back to what is drawn without it.
        struct FieldSurface
        {
            uint64_t       Key = 0;
            LazyFieldInput Input;
        };

        FieldSurface DeriveDisplacedSurface( const std::filesystem::path& path, uint64_t key,
                                             const std::vector<uint8_t>&                        sided,
                                             std::shared_ptr<const MeshDistanceFieldBuildInput> base,
                                             const std::optional<DisplacementPlan>&             plan,
                                             DisplacementTopologySlot*                          topologySlot,
                                             DisplacedMeshSlot* displacedSlot, std::string& error )
        {
            // Undisplaced, the cards and the field are keyed on the triangles they are built from (the content
            // key), not on the render data: a mesh builder change that leaves those triangles alone keeps the
            // cache.
            FieldSurface surface{ MeshDistanceFieldInputContentKey( *base ),
                                  [base]() -> Common::ResultStr<MeshDistanceFieldBuildInput>
                                  { return Common::MakeSuccess( *base ); } };
            if ( !plan.has_value() )
                return surface;
            const std::shared_ptr<DisplacedMeshBuildInput>& displacedInput = plan->Input;
            // The topology every displaced product dices from (cooked from the render data).
            auto topology = LoadDisplacementTopology(
                 path, key,
                 [&displacedInput]() -> Common::ResultStr<DisplacementTopologyBuildInput>
                 {
                     return Common::MakeSuccess( DisplacementTopologyBuildInput{
                          displacedInput->Vertices, displacedInput->Indices, displacedInput->Submeshes } );
                 } );
            if ( !topology.IsSuccess() )
            {
                error = std::format( "{}; the mesh is drawn undisplaced", topology.GetError() );
                if ( displacedSlot )
                    displacedSlot->Fail();
                return surface;
            }
            displacedInput->Topology = topology.ExtractValue();
            if ( topologySlot )
                topologySlot->Publish( displacedInput->Topology );
            if ( plan->TopologyOnly )
                return surface; // nothing on the asset moves: its surface is the render data
            auto baked = LoadDisplacedMesh( path, plan->DisplacedKey,
                                            [&displacedInput]() -> Common::ResultStr<DisplacedMeshBuildInput>
                                            { return Common::MakeSuccess( *displacedInput ); } );
            if ( baked.IsSuccess() )
            {
                if ( displacedSlot )
                    displacedSlot->Publish( baked.GetValue() );
            }
            else
            {
                if ( displacedSlot )
                    displacedSlot->Fail();
                error =
                     std::format( "{}; the mesh has no displaced surface (collision and the Baked draw keep the "
                                  "base mesh)",
                                  baked.GetError() );
            }
            if ( plan->Mode == MeshDisplacementMode::Baked )
            {
                if ( !baked.IsSuccess() )
                    return surface; // Baked without its mesh: the field of what is drawn (the base)
                const auto bakedMesh = baked.GetValue();
                surface.Key          = plan->SurfaceKey;
                surface.Input        = [bakedMesh, sided]() -> Common::ResultStr<MeshDistanceFieldBuildInput> {
                    return MeshDistanceFieldInputOf( bakedMesh->Vertices, bakedMesh->Indices, bakedMesh->Submeshes,
                                                            sided );
                };
                return surface;
            }
            // Runtime: the maximum level, built only on a miss of the cards or the field (not stored).
            surface.Key   = plan->SurfaceKey;
            surface.Input = [path, displacedInput, sided,
                             surfaceKey = plan->SurfaceKey]() -> Common::ResultStr<MeshDistanceFieldBuildInput>
            {
                DisplacedMeshBuildInput maxLevel = *displacedInput;
                maxLevel.EdgeLengthCm            = std::nullopt;
                maxLevel.BakeLODs                = false;
                auto densest                     = BuildDisplacedMesh( path, surfaceKey, maxLevel );
                if ( !densest.IsSuccess() )
                    return Common::MakeError<MeshDistanceFieldBuildInput>( densest.GetError() );
                const DisplacedMeshData& mesh = *densest.GetValue();
                return MeshDistanceFieldInputOf( mesh.Vertices, mesh.Indices, mesh.Submeshes, sided );
            };
            return surface;
        }
    } // namespace

    void StaticMeshAsset::ResolveDependencies( AssetManager& manager )
    {
        m_DistanceField.reset();
        m_Cards.reset();
        m_DisplacementTopology.reset();
        m_DisplacedMesh.reset();
        // An unread shell has no render data yet; AssetBase::EnsureLoaded runs this again after the read.
        if ( !m_IsReadyForUse )
            return;
        m_DependenciesResolved = true;

        // EVERY SUBMESH'S MATERIAL SIDEDNESS and the triangles (ResolveFieldInputs), the displacement of every
        // section (ResolveDisplacementPlan), then THE MESH'S DISPLACED SURFACE, CARDS AND DISTANCE FIELD, derived
        // from exactly this render data and these materials - a DDC hit, or built where the editor registered the
        // builders - ON A WORKER (UE FDistanceFieldAsyncQueue): the mesh draws at once and joins the
        // distance-field scene when its field lands in the slot. One that cannot be had is an error naming the
        // mesh (the queue logs it), never a field guessed from a different input. The job owns copies of what it
        // reads: this asset may be reloaded or unloaded while it runs, and a reload makes a new slot, so a late
        // field lands in a slot nothing reads any more.
        EnsureCpuGeometry(); // a re-resolve after the GPU upload reads the triangles back
        auto inputs = ResolveFieldInputs( manager, m_MaterialGuids, m_Vertices, m_Indices, m_Submeshes );
        if ( !inputs )
        {
            LOG_ERROR( "StaticMeshAsset '{}': {}; the mesh is left out of every distance-field trace",
                       m_Metadata.Filepath.string(), inputs.GetError() );
            return;
        }
        std::optional<DisplacementPlan> plan;
        if ( auto resolved = ResolveDisplacementPlan( manager, m_ImportSettings, m_RenderDataKey, m_MaterialGuids,
                                                      m_Vertices, m_VertexStreams, m_Indices, m_Submeshes );
             resolved.IsSuccess() )
            plan = resolved.ExtractValue();
        else
            LOG_ERROR( "StaticMeshAsset '{}': {}; the mesh is drawn undisplaced", m_Metadata.Filepath.string(),
                       resolved.GetError() );
        std::shared_ptr<DisplacementTopologySlot> topologySlot;
        std::shared_ptr<DisplacedMeshSlot>        displacedSlot;
        if ( plan.has_value() )
        {
            if ( plan->Mode == MeshDisplacementMode::Runtime )
            {
                topologySlot           = std::make_shared<DisplacementTopologySlot>();
                m_DisplacementTopology = topologySlot;
            }
            if ( !plan->TopologyOnly )
            {
                displacedSlot   = std::make_shared<DisplacedMeshSlot>();
                m_DisplacedMesh = displacedSlot;
            }
        }
        FieldInputs in   = inputs.ExtractValue();
        auto        slot = std::make_shared<MeshDistanceFieldSlot>( MostlyTwoSided( in.Input ) );
        m_DistanceField  = slot;
        auto cards       = std::make_shared<MeshCardsSlot>();
        m_Cards          = cards;
        EnqueueMeshDistanceField(
             slot, m_Metadata.Filepath.string(),
             [path = m_Metadata.Filepath, key = m_RenderDataKey, sided = std::move( in.SubmeshTwoSided ),
              input = std::make_shared<const MeshDistanceFieldBuildInput>( std::move( in.Input ) ), cards,
              plan  = std::move( plan ), topologySlot,
              displacedSlot]( bool& built, const MeshDistanceFieldCancel& cancel )
                  -> Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>>
             {
                 std::string        displacementError;
                 const FieldSurface surface = DeriveDisplacedSurface(
                      path, key, sided, input, plan, topologySlot.get(), displacedSlot.get(), displacementError );
                 if ( !displacementError.empty() )
                     LOG_ERROR( "StaticMeshAsset '{}': {}", path.string(), displacementError );
                 // A card failure leaves the mesh without cards - out of the surface cache, lit in place by a
                 // trace that hits it - and is logged naming the mesh, never guessed.
                 return DeriveCardsThenField(
                      path, surface.Key, sided, surface.Input, cancel, built,
                      [&path, &cards]( Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>> loaded )
                      {
                          if ( loaded.IsSuccess() )
                              cards->Publish( loaded.ExtractValue() );
                          else
                              LOG_ERROR( "StaticMeshAsset '{}': {}; the mesh has no surface-cache cards",
                                         path.string(), loaded.GetError() );
                      } );
             } );
    }

    Common::BoolResultStr StaticMeshAsset::DeriveDistanceFieldAndCardsNow( AssetManager& manager ) const
    {
        if ( !m_IsReadyForUse )
            return Common::MakeError(
                 std::format( "static mesh '{}' has no render data loaded", m_Metadata.Filepath.string() ) );
        EnsureCpuGeometry();
        auto inputs = ResolveFieldInputs( manager, m_MaterialGuids, m_Vertices, m_Indices, m_Submeshes );
        if ( !inputs )
            return Common::MakeError( std::string( inputs.GetError() ) );
        // The displaced products the packaged game loads (DisplacementTopology, DisplacedStaticMesh buckets) and
        // the cards and field keyed from the displaced surface - the same keys ResolveDependencies' job asks for.
        auto plan = ResolveDisplacementPlan( manager, m_ImportSettings, m_RenderDataKey, m_MaterialGuids,
                                             m_Vertices, m_VertexStreams, m_Indices, m_Submeshes );
        if ( !plan )
            return Common::MakeError( "displacement: " + std::string( plan.GetError() ) );
        FieldInputs        in = inputs.ExtractValue();
        std::string        displacementError;
        const FieldSurface surface =
             DeriveDisplacedSurface( m_Metadata.Filepath, m_RenderDataKey, in.SubmeshTwoSided,
                                     std::make_shared<const MeshDistanceFieldBuildInput>( std::move( in.Input ) ),
                                     plan.GetValue(), nullptr, nullptr, displacementError );
        if ( !displacementError.empty() )
            return Common::MakeError( "displacement: " + displacementError );
        std::string cardsError;
        bool        built = false;
        const auto  field = DeriveCardsThenField(
             m_Metadata.Filepath, surface.Key, in.SubmeshTwoSided, surface.Input, MeshDistanceFieldCancel{}, built,
             [&cardsError]( Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>> loaded )
             {
                 if ( !loaded.IsSuccess() )
                     cardsError = loaded.GetError();
             } );
        if ( !cardsError.empty() )
            return Common::MakeError( "cards: " + cardsError );
        if ( !field )
            return Common::MakeError( "distance field: " + std::string( field.GetError() ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
#include "StaticMeshAsset.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
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

        m_Vertices.reserve( data.StaticVertices.size() );
        m_Indices.reserve( data.Indices.size() );
        m_Submeshes.reserve( data.Submeshes.size() );
        m_MaterialAssetHandles.reserve( data.Submeshes.size() );

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
        {
            Index index;
            index.V1 = i.V1;
            index.V2 = i.V2;
            index.V3 = i.V3;

            m_Indices.emplace_back( index );
        }

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

            // Baked LOD chain (if the asset was cooked with LODs): copy the simplified triangle sets so
            // StaticMesh appends them instead of re-simplifying at load. Empty -> generated at load.
            submesh.BakedLODs.reserve( s.LODs.size() );
            for ( const auto& lvl : s.LODs )
            {
                std::vector<Index> tris;
                tris.reserve( lvl.size() );
                for ( const auto& t : lvl )
                    tris.push_back( { t.V1, t.V2, t.V3 } );
                submesh.BakedLODs.push_back( std::move( tris ) );
            }

            m_MaterialGuids.push_back( s.MaterialGuid );
            m_MaterialAssetHandles.emplace_back(
                 s.MaterialGuid.IsNull() ? Common::UUID::Null()
                                         : Common::UUID( static_cast<uint64_t>(
                                                Common::Content::HandleForGuid( s.MaterialGuid ) ) ) );
            m_Submeshes.emplace_back( std::move( submesh ) );
        }

        m_MorphTargets.clear();
        m_MorphTargets.reserve( data.MorphTargets.size() );
        for ( const auto& mt : data.MorphTargets )
            m_MorphTargets.push_back( MorphTarget{ mt.Name, mt.DeltaPositions, mt.DeltaNormals } );
        m_VertexStreams = PackMeshVertexStreams( data.Colors, data.UV1, m_Vertices.size() );

        // The distance field is keyed on this render data; ResolveDependencies derives it once the materials load.
        m_RenderDataKey = raw.GetValue().Key;
        m_DistanceField.reset();
        m_Cards.reset();
        m_DependenciesResolved = false;

        m_IsReadyForUse = true;
        return BOOLSUCCESS;
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
        m_MaterialGuids.clear();
        m_MaterialGuids.shrink_to_fit();
        m_DependenciesResolved = false;

        m_Vertices.shrink_to_fit();
        m_Indices.shrink_to_fit();
        m_Submeshes.shrink_to_fit();
        m_MorphTargets.shrink_to_fit();
        m_VertexStreams.clear();
        m_VertexStreams.shrink_to_fit();
        m_MaterialAssetHandles.shrink_to_fit();

        // Same reason as SkinnedMeshAsset::Unload: the flag is what EnsureLoaded asks before deciding to
        // parse, so an emptied asset that still reports "ready" is one nobody will ever reload.
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }

    void StaticMeshAsset::ResolveDependencies( AssetManager& manager )
    {
        m_DistanceField.reset();
        m_Cards.reset();
        // An unread shell has no render data yet; AssetBase::EnsureLoaded runs this again after the read.
        if ( !m_IsReadyForUse )
            return;
        m_DependenciesResolved = true;

        // EVERY SUBMESH'S MATERIAL SIDEDNESS, the field's second input. A slot naming no material draws the
        // engine's Default Surface (MeshECSSystem::DefaultInstance), a one-sided template.
        std::vector<uint8_t> twoSided( m_MaterialGuids.size(), 0 );
        for ( size_t i = 0; i < m_MaterialGuids.size(); ++i )
        {
            if ( m_MaterialGuids[i].IsNull() )
                continue;
            const auto sided = ResolveMaterialTwoSided( manager, m_MaterialGuids[i] );
            if ( !sided )
            {
                LOG_ERROR( "StaticMeshAsset '{}': submesh {} has no distance field material sidedness ({}); the "
                           "mesh is left out of every distance-field trace",
                           m_Metadata.Filepath.string(), i, sided.GetError() );
                return;
            }
            twoSided[i] = sided.GetValue() ? 1 : 0;
        }

        // THE MESH'S DISTANCE FIELD, derived from exactly this render data and these materials - a DDC hit, or
        // built where the editor registered the builder - ON A WORKER (UE FDistanceFieldAsyncQueue): the mesh
        // draws at once and joins the distance-field scene when its field lands in the slot. One that cannot be
        // had is an error naming the mesh (the queue logs it), never a field guessed from a different input.
        // The job owns copies of what it reads: this asset may be reloaded or unloaded while it runs, and a
        // reload makes a new slot, so a late field lands in a slot nothing reads any more.
        auto input = MeshDistanceFieldInputOf( m_Vertices, m_Indices, m_Submeshes, twoSided );
        if ( !input )
        {
            LOG_ERROR( "StaticMeshAsset '{}': {}; the mesh is left out of every distance-field trace",
                       m_Metadata.Filepath.string(), input.GetError() );
            return;
        }
        auto slot       = std::make_shared<MeshDistanceFieldSlot>( MostlyTwoSided( input.GetValue() ) );
        m_DistanceField = slot;
        auto cards      = std::make_shared<MeshCardsSlot>();
        m_Cards         = cards;
        EnqueueMeshDistanceField(
             slot, m_Metadata.Filepath.string(),
             [path = m_Metadata.Filepath, key = m_RenderDataKey, sided = std::move( twoSided ),
              in = std::make_shared<MeshDistanceFieldBuildInput>( input.ExtractValue() ),
              cards]( bool& built, const MeshDistanceFieldCancel& cancel )
                  -> Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>>
             {
                 // The cards first (UE FCardRepresentationAsyncQueue, here the same job): a DDC hit or a build
                 // from the same triangles. A failure leaves the mesh without cards - out of the surface cache,
                 // lit in place by a trace that hits it - and is logged naming the mesh, never guessed.
                 // A cancelled card build is the job's cancel (DfRun: not a failure, nothing logged; a flight
                 // that followed another owner's cancelled leader is derived again).
                 if ( auto loaded = LoadMeshCards( path, key, sided, *in, cancel ); loaded.IsSuccess() )
                     cards->Publish( loaded.ExtractValue() );
                 else if ( loaded.GetError().find( kMeshCardsCancelled ) != std::string::npos )
                     return Common::MakeError<std::shared_ptr<const MeshDistanceFieldData>>(
                          std::string( kMeshDistanceFieldCancelled ) );
                 else
                     LOG_ERROR( "StaticMeshAsset '{}': {}; the mesh has no surface-cache cards", path.string(),
                                loaded.GetError() );
                 return LoadMeshDistanceField(
                      path, key, sided, [&in]() -> Common::ResultStr<MeshDistanceFieldBuildInput>
                      { return Common::MakeSuccess( *in ); }, &built, cancel );
             } );
    }

} // namespace Desert::Assets
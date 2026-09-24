#include "StaticMeshAsset.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    StaticMeshAsset::StaticMeshAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : MeshAsset( priority, filepath, GetTypeID() )
    {
        // The path-derived handle this type used to compute for itself (twice — here and again in Load)
        // now comes from AssetBase, which derives it the same way for every asset type. See the comment on
        // that constructor.
    }

    Common::BoolResultStr StaticMeshAsset::LoadFromFile()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );

        // ONE READER, ONE FORM. A cooked mesh is a binary container; the JSON arm that used to open a
        // clone's older cooks was removed by owner decision — cooked content is DERIVED, so a stale one
        // is deleted and cooked again rather than migrated. Neither this class nor its skinned twin
        // knows how the bytes are laid out, and that is the point: one place decides, and it is
        // testable without a filesystem.
        const auto dataReflected =
             Serialization::ReadMeshAssetData( raw.GetValue(), m_Metadata.Filepath.string() );
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

        m_Vertices.shrink_to_fit();
        m_Indices.shrink_to_fit();
        m_Submeshes.shrink_to_fit();
        m_MorphTargets.shrink_to_fit();
        m_MaterialAssetHandles.shrink_to_fit();

        // Same reason as SkinnedMeshAsset::Unload: the flag is what EnsureLoaded asks before deciding to
        // parse, so an emptied asset that still reports "ready" is one nobody will ever reload.
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }

} // namespace Desert::Assets
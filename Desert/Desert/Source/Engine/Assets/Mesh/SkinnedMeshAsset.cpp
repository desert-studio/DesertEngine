#include "SkinnedMeshAsset.hpp"
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    SkinnedMeshAsset::SkinnedMeshAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : MeshAsset( priority, filepath, GetTypeID() )
    {
        // The path-derived handle this type used to compute for itself (twice — here and again in Load)
        // now comes from AssetBase, which derives it the same way for every asset type. See the comment on
        // that constructor.
    }

    Common::BoolResultStr SkinnedMeshAsset::LoadFromFile()
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

        if ( !data.IsSkinned )
        {
            return Common::MakeError( "SkinnedMeshAsset cannot load static mesh data." );
        }

        // The same refusal StaticMeshAsset::Load states in full, for the same reason and in the same place:
        // a mesh with no submeshes issues no draw, and a success that means "empty" cannot be told from a
        // success that means "loaded".
        if ( data.Submeshes.empty() )
        {
            return Common::MakeFormattedError(
                 "'{}' parsed but carries ZERO submeshes ({} vertices, {} triangles), so nothing in it can "
                 "be drawn. The cooked file is incomplete — re-cook it (Assets > Rebuild Cooked Assets).",
                 m_Metadata.Filepath.string(), data.SkinnedVertices.size(), data.Indices.size() );
        }

        m_Vertices.clear();
        m_Indices.clear();
        m_Submeshes.clear();
        // Cleared for the reason StaticMeshAsset::Load records in full: the loop below appends one handle
        // per submesh, so a second Load without this doubles the vector and leaves the STALE handles in
        // the indices every draw reads.
        m_MaterialAssetHandles.clear();

        m_Vertices.reserve( data.SkinnedVertices.size() );
        m_Indices.reserve( data.Indices.size() );
        m_Submeshes.reserve( data.Submeshes.size() );

        for ( const auto& v : data.SkinnedVertices )
        {
            SkinnedVertex vertex;

            vertex.StaticVertex.Position  = v.Position;
            vertex.StaticVertex.Normal    = v.Normal;
            vertex.StaticVertex.Tangent   = v.Tangent;
            vertex.StaticVertex.Bitangent = v.Bitangent;
            vertex.StaticVertex.TexCoord  = v.TexCoord;

            vertex.BoneIDs     = v.BoneIDs;
            vertex.BoneWeights = v.BoneWeights;

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

            // NEVER FILLED HERE UNTIL Д19, while a per-index accessor indexed it — so every material
            // lookup on a skinned mesh was an out-of-bounds read of an EMPTY vector. StaticMeshAsset::Load
            // has always had this line; the skinned path simply never grew it, and the two classes
            // implement the same pure virtual. Found while giving Unload a caller: Unload cleared a vector
            // Load never wrote.
            //
            // The per-index accessor itself is gone (Г12 — it had no callers), so this line's job is now
            // the plural `GetMaterialHandles()`: it must answer with one handle per submesh, and it can
            // only do that if this loop writes one. The emptiness that was a crash is now a silence, which
            // is why the line still matters.
            m_MaterialAssetHandles.emplace_back( s.MaterialHandle );
            m_Submeshes.emplace_back( std::move( submesh ) );
        }

        if ( !data.SkeletonSignature.has_value() )
        {
            return Common::MakeError( "SkinnedMeshAsset requires SkeletonUUID." );
        }

        m_SkeletonSignature = data.SkeletonSignature.value();

        m_MorphTargets.clear();
        m_MorphTargets.reserve( data.MorphTargets.size() );
        for ( const auto& mt : data.MorphTargets )
            m_MorphTargets.push_back( MorphTarget{ mt.Name, mt.DeltaPositions, mt.DeltaNormals } );

        // StaticMeshAsset::Load has always ended this way; this one never did, so IsReadyForUse stayed false
        // for the whole session and every `if (!IsReadyForUse()) Load()` in the engine re-read and re-parsed
        // the entire .skmesh. MeshService::GetAsset is on the per-frame path, so the cost was a full JSON
        // parse of the character PER FRAME, silently, on top of the mesh being invisible.
        m_IsReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SkinnedMeshAsset::Unload()
    {
        m_Vertices.clear();
        m_Indices.clear();
        m_Submeshes.clear();
        m_MorphTargets.clear();
        m_MaterialAssetHandles.clear();

        m_Vertices.shrink_to_fit();
        m_Indices.shrink_to_fit();
        m_Submeshes.shrink_to_fit();
        m_MorphTargets.shrink_to_fit();
        m_MaterialAssetHandles.shrink_to_fit();

        // THE RIG SIGNATURE AND ITS DEPENDENCY GO WITH THE PAYLOAD. `ResolveDependencies` matches a
        // SkeletonAsset by `GetSignature() == m_SkeletonSignature`, and `GetSkeletonDependency().IsValid()`
        // is what callers ask before using the rig — an unloaded mesh answering both as if it were loaded
        // is the same contradiction between a readiness flag and a getter that the cloud type had. The
        // resolve re-runs on the next EnsureLoaded, which is written to be re-runnable.
        m_SkeletonSignature  = 0U;
        m_SkeletonDependency = AssetDependency<SkeletonAsset>{};

        // The flag is what EnsureLoaded asks before deciding to parse, so an emptied asset that still
        // reports "ready" is an asset nobody will ever reload — the same never-recovers shape as the
        // dependency this file just stopped losing.
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
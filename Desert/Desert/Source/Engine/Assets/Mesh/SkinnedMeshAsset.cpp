#include "SkinnedMeshAsset.hpp"
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Common/Core/Logger.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    SkinnedMeshAsset::SkinnedMeshAsset( const Common::Filepath& filepath ) : MeshAsset( filepath, GetTypeID() )
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
            m_MaterialAssetHandles.emplace_back(
                 s.MaterialGuid.IsNull() ? Common::UUID::Null()
                                         : Common::UUID( static_cast<uint64_t>(
                                                Common::Content::HandleForGuid( s.MaterialGuid ) ) ) );
            m_Submeshes.emplace_back( std::move( submesh ) );
        }

        if ( data.Skeleton.IsNull() )
        {
            return Common::MakeFormattedError( "skinned mesh '{}' names no skeleton (MeshBinary v5 "
                                                     "SkeletonGuid is null) - it cannot be skinned",
                                                     m_Metadata.Filepath.string() );
        }

        m_Skeleton = data.Skeleton;

        m_MorphTargets.clear();
        m_MorphTargets.reserve( data.MorphTargets.size() );
        for ( const auto& mt : data.MorphTargets )
            m_MorphTargets.push_back( MorphTarget{ mt.Name, mt.DeltaPositions, mt.DeltaNormals } );
        m_VertexStreams = PackMeshVertexStreams( data.Colors, data.UV1, m_Vertices.size() );

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
        m_VertexStreams.clear();
        m_VertexStreams.shrink_to_fit();
        m_MaterialAssetHandles.shrink_to_fit();

        // THE SKELETON REFERENCE AND ITS DEPENDENCY GO WITH THE PAYLOAD. `ResolveDependencies` binds the
        // SkeletonAsset of `m_Skeleton`, and `GetSkeletonDependency().IsValid()`
        // is what callers ask before using the rig — an unloaded mesh answering both as if it were loaded
        // is the same contradiction between a readiness flag and a getter that the cloud type had. The
        // resolve re-runs on the next EnsureLoaded, which is written to be re-runnable.
        m_Skeleton           = {};
        m_SkeletonDependency = AssetDependency<SkeletonAsset>{};

        // The flag is what EnsureLoaded asks before deciding to parse, so an emptied asset that still
        // reports "ready" is an asset nobody will ever reload — the same never-recovers shape as the
        // dependency this file just stopped losing.
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }
    void SkinnedMeshAsset::ResolveDependencies( AssetManager& manager )
    {
        m_SkeletonDependency.Handle = Common::AssetHandle::Null();
        m_SkeletonDependency.Cached.reset();

        // A NULL GUID IS "NOT PARSED YET" (or a mesh naming no skeleton, which Load refuses): nothing is bound.
        // AssetBase::EnsureLoaded runs this again once the parse fills the reference in.
        if ( m_Skeleton.IsNull() )
            return;

        // BY GUID, the rig's identity (SkeletonAsset adopts HandleForGuid of its header GUID), as
        // RetargetAsset::ResolveDependencies: a rig re-cooked, renamed or moved still resolves.
        auto skeleton = manager.FindByHandle<SkeletonAsset>(
             Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( m_Skeleton ) ) ) );
        if ( !skeleton )
            skeleton = CreateFromRegistryGuid<SkeletonAsset>( manager, m_Skeleton,
                                                              Common::Content::ContentKind::Skeleton );
        if ( !skeleton )
        {
            LOG_WARN( "SkinnedMeshAsset '{}': skeleton {} is not a skeleton this project has scanned.",
                      m_Metadata.Filepath.string(), Common::Content::AssetGuidToText( m_Skeleton ) );
            return;
        }

        // THE RIG'S BONES MUST BE RESIDENT BEFORE THIS COUNTS AS RESOLVED: MeshFactory::CreateSkinned reads
        // them and refuses the mesh without them, and eviction leaves a rig cold whenever no scene holds it.
        if ( const auto loaded = skeleton->EnsureLoaded( manager ); !loaded )
        {
            LOG_ERROR( "SkinnedMeshAsset '{}': skeleton '{}' could not be read: {}", m_Metadata.Filepath.string(),
                       skeleton->GetMetadata().Filepath.string(), loaded.GetError() );
            return;
        }

        m_SkeletonDependency.Handle = skeleton->GetMetadata().Handle;
        m_SkeletonDependency.Cached = skeleton;
    }

    Common::Content::AssetGuid SkinnedMeshAsset::GetSkeleton() const
    {
        return m_Skeleton;
    }

    void SkinnedMeshAsset::SetSkeleton( const Common::Content::AssetGuid skeleton )
    {
        m_Skeleton = skeleton;
    }
} // namespace Desert::Assets
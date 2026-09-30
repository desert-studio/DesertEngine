#include "AnimationLibrary.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>

namespace Desert::Animation
{
    AnimationLibrary::AnimationLibrary( Assets::AssetManager* assetManager ) : m_AssetManager( assetManager )
    {
    }

    // RELOAD BEFORE HANDING ONE OUT. The lookup resolves a handle the record kept at Register time, and an
    // evicted clip resolves to a perfectly valid asset holding an empty track list — so without this the
    // caller gets a successful answer that animates nothing. AssetBase::EnsureLoaded is a no-op for a clip
    // that is already resident, which is every clip in the common case.
    //
    // A clip that CANNOT be reloaded is named and skipped rather than returned empty: a procedural clip
    // (SetInMemoryClip) is never evicted, so reaching this branch means the file is gone.
    Assets::Asset<Assets::AnimationAsset> AnimationLibrary::Resolve( const Assets::AssetHandle& handle ) const
    {
        auto asset = m_AssetManager->FindByHandle<Assets::AnimationAsset>( handle );
        if ( !asset )
            return nullptr;
        // An evicted clip is read again by the loader, never here: a synchronous reload inside the
        // animation update is a hitch in the frame (plan 2.4). Until it arrives the lookup is pending.
        if ( !asset->IsReadyForUse() )
        {
            RequestRead( asset );
            return nullptr;
        }
        return asset;
    }

    void AnimationLibrary::RequestRead( const Assets::Asset<Assets::AnimationAsset>& asset ) const
    {
        const Assets::AssetHandle handle = asset->GetMetadata().Handle;
        if ( m_Requests.contains( handle ) )
            return;
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             asset,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 m_Requests.erase( handle );
                 std::erase_if( m_Unread, [&]( const UnreadRow& row ) { return row.Handle == handle; } );
                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     LOG_ERROR( "[AnimationLibrary] clip '{}' could not be read: {}. It is not offered.",
                                loaded->GetMetadata().Filepath.string(), error );
                     return;
                 }
                 const bool known = std::any_of( m_Clips.begin(), m_Clips.end(),
                                                 [&]( const ClipRigIdentity& c ) { return c.Handle == handle; } );
                 // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                 auto* self = const_cast<AnimationLibrary*>( this );
                 if ( !known )
                     self->Register( std::static_pointer_cast<Assets::AnimationAsset>( loaded ) );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    void AnimationLibrary::CatchUpWrites() const
    {
        const auto written =
             Assets::ContentRegistry::WrittenSince( Common::Content::ContentKind::Animation, m_SeenWrites );
        m_SeenWrites = written.Serial;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto* self = const_cast<AnimationLibrary*>( this );
        for ( const Assets::ContentRegistry::PickerRow& row : written.Rows )
        {
            self->Unregister( row.Handle );
            std::erase_if( m_Unread, [&]( const UnreadRow& r ) { return r.Handle == row.Handle; } );
            const auto resident = m_AssetManager->ProbeByHandle<Assets::AnimationAsset>( row.Handle );
            if ( resident && resident->IsReadyForUse() && !m_Requests.contains( row.Handle ) )
                self->Register( resident );
            else
                m_Unread.push_back( { row.Handle, row.DisplayName } );
        }
    }

    void AnimationLibrary::RequestUnread( const std::string& clipName ) const
    {
        CatchUpWrites();
        // Copied: a read that completes inside Request would edit m_Unread under the loop.
        const std::vector<UnreadRow> unread = m_Unread;
        for ( const UnreadRow& row : unread )
        {
            if ( !clipName.empty() && !row.ClipName.empty() && row.ClipName != clipName )
                continue;
            if ( m_Requests.contains( row.Handle ) )
                continue;
            auto asset = m_AssetManager->ProbeByHandle<Assets::AnimationAsset>( row.Handle );
            if ( !asset )
            {
                if ( const auto located = Assets::ContentRegistry::RowOf( Common::Content::ContentKind::Animation,
                                                                          static_cast<uint64_t>( row.Handle ) ) )
                    asset = m_AssetManager->CreateAsset<Assets::AnimationAsset>( located->Path,
                                                                                 /*loadAfterCreate=*/false );
            }
            if ( !asset )
            {
                LOG_ERROR( "[AnimationLibrary] clip row '{}' (handle {}) is indexed but its asset could not be "
                           "created; it is dropped from the library.",
                           row.ClipName, static_cast<uint64_t>( row.Handle ) );
                std::erase_if( m_Unread, [&]( const UnreadRow& r ) { return r.Handle == row.Handle; } );
                continue;
            }
            RequestRead( asset );
        }
    }

    size_t AnimationLibrary::IndexRegistryRows()
    {
        // Taken before the rows: a write racing the read is caught up again, never lost.
        m_SeenWrites   = Assets::ContentRegistry::WriteSerial();
        size_t indexed = 0;
        for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ) )
        {
            if ( row.DisplayName.empty() )
                LOG_ERROR( "[AnimationLibrary] clip file '{}' states no Name; only a lookup naming no clip "
                           "reads it.",
                           row.Path.string() );
            m_Unread.push_back( { row.Handle, row.DisplayName } );
            ++indexed;
        }
        return indexed;
    }

    bool AnimationLibrary::HasPending( const std::string& clipName ) const
    {
        CatchUpWrites();
        const auto named = [&]( const std::string& name )
        { return clipName.empty() || name.empty() || name == clipName; };
        return std::any_of( m_Unread.begin(), m_Unread.end(),
                            [&]( const UnreadRow& row ) { return named( row.ClipName ); } ) ||
               std::any_of( m_Clips.begin(), m_Clips.end(), [&]( const ClipRigIdentity& c )
                            { return named( c.ClipName ) && m_Requests.contains( c.Handle ); } );
    }

    void AnimationLibrary::Register( const Assets::Asset<Assets::AnimationAsset>& animation )
    {
        if ( !animation )
        {
            return;
        }

        ClipRigIdentity identity;
        identity.Handle   = animation->GetMetadata().Handle;
        identity.ClipName = animation->GetClip().AnimationName;
        identity.Skeleton = SkeletonRefOf( animation->GetSkeleton() );

        // A clip that references no skeleton plays nowhere (ClipPlaysOnMesh refuses it). Registering it
        // silently is how a clip becomes invisible with no way to tell that from "the project has no clips".
        if ( identity.Skeleton.Guid.IsNull() )
        {
            LOG_ERROR( "[AnimationLibrary] clip '{}' ({}) references no skeleton, so it plays on no mesh. It is "
                       "registered and will never be offered.",
                       identity.ClipName, animation->GetMetadata().Filepath.string() );
        }

        m_Clips.push_back( std::move( identity ) );
    }

    void AnimationLibrary::Unregister( const Assets::AssetHandle& handle )
    {
        m_Clips.erase( std::remove_if( m_Clips.begin(), m_Clips.end(),
                                       [&]( const ClipRigIdentity& c ) { return c.Handle == handle; } ),
                       m_Clips.end() );
    }

    SkeletonAssetRef AnimationLibrary::SkeletonRefOf( const Common::Content::AssetGuid& skeleton )
    {
        SkeletonAssetRef ref;
        ref.Guid = skeleton;
        if ( skeleton.IsNull() )
            ref.Name = "(none)";
        else if ( const auto row = Assets::ContentRegistry::RigRow( skeleton ) )
            ref.Name = row->Key;
        else
            ref.Name = Common::Content::AssetGuidToText( skeleton );
        return ref;
    }

    MeshSkeletonIdentity AnimationLibrary::IdentifyMesh( const Assets::SkinnedMeshAsset& mesh )
    {
        MeshSkeletonIdentity identity;
        identity.Skeleton = SkeletonRefOf( mesh.GetSkeleton() );
        if ( const auto skeleton = mesh.GetSkeletonDependency().Cached.lock() )
        {
            const auto compatible = skeleton->GetCompatibleSkeletons();
            identity.Compatible.assign( compatible.begin(), compatible.end() );
        }
        return identity;
    }

    MeshSkeletonIdentity AnimationLibrary::IdentifyMeshHandle( const Assets::AssetHandle& mesh ) const
    {
        if ( m_AssetManager != nullptr )
            if ( const auto asset =
                      m_AssetManager->ProbeByHandle<Assets::SkinnedMeshAsset>( Common::UUID( mesh ) ) )
                return IdentifyMesh( *asset );
        return MeshSkeletonIdentity{ SkeletonRefOf( {} ), {} };
    }

    std::vector<Assets::Asset<Assets::AnimationAsset>>
    AnimationLibrary::GetForMesh( const MeshSkeletonIdentity& mesh ) const
    {
        RequestUnread( {} );

        std::vector<Assets::Asset<Assets::AnimationAsset>> result;
        for ( const size_t i : SelectClipsForMesh( m_Clips, mesh ) )
        {
            if ( auto asset = Resolve( m_Clips[i].Handle ) )
                result.push_back( asset );
        }
        return result;
    }

    Common::ResultStr<Assets::Asset<Assets::AnimationAsset>>
    AnimationLibrary::FindForMesh( const MeshSkeletonIdentity& mesh, const std::string& clipName ) const
    {
        RequestUnread( clipName );

        const auto index = FindClipForMesh( m_Clips, mesh, clipName );
        if ( !index )
            return Common::MakeError<Assets::Asset<Assets::AnimationAsset>>( index.GetError() );

        auto asset = Resolve( m_Clips[index.GetValue()].Handle );
        if ( !asset )
        {
            // Resolve already logged the reason; this turns it into a refusal the caller must handle rather
            // than a null it can drop on the floor.
            return Common::MakeFormattedError<Assets::Asset<Assets::AnimationAsset>>(
                 "clip '{}' plays on this mesh but its asset could not be resolved or reloaded.", clipName );
        }
        return Common::MakeSuccess( std::move( asset ) );
    }

    void AnimationLibrary::Clear()
    {
        m_Clips.clear();
        m_Unread.clear();
        m_Requests.clear();
    }

    Common::ResultStr<LibraryPopulation> PopulateLibrary( Assets::AssetManager& /*assets*/, AnimationLibrary& library,
                                                          const size_t clipFilesDiscovered )
    {
        // CLEARED FIRST because this is also the re-index path: `Assets::IndexAnimationClips` from ("Rebuild
        // Cooked Assets") runs the whole discovery again, and a library that only ever grew would answer
        // with two records per clip afterwards — the second of which resolves the same handle, so nothing
        // would look wrong until a picker showed every clip twice.
        library.Clear();

        LibraryPopulation counts;

        counts.FromFiles = library.IndexRegistryRows();

        LOG_INFO( "[AnimationLibrary] {} clip(s) registered from {} `.anim` file(s) on disk.", counts.FromFiles,
                  clipFilesDiscovered );

        // THE CASE THAT SHIPPED, said out loud. An empty library is the correct state for a project with no
        // clips and a broken one for a project with clips on disk, and only the scan's own count can tell
        // the two apart — which is why it is a parameter. Without this line the symptom is a character
        // standing still and no log line anywhere in the process.
        if ( clipFilesDiscovered > 0 && counts.FromFiles == 0 )
        {
            return Common::MakeFormattedError<LibraryPopulation>(
                 "the asset scan found {} `.anim` file(s) under the cooked mesh root and NOT ONE of them "
                 "reached the animation library. Every skinned character whose clip comes from a file will "
                 "stand in its bind pose.",
                 clipFilesDiscovered );
        }

        // Fewer than were found is a real loss too — a file that failed to parse never became an asset —
        // but it is a partial one, and the per-file reason is already on the log from AnimationAsset::Load.
        if ( counts.FromFiles < clipFilesDiscovered )
        {
            LOG_WARN( "[AnimationLibrary] {} of {} `.anim` file(s) did not become a clip asset and are not "
                      "in the library; the reason for each is logged above by the asset load that failed.",
                      clipFilesDiscovered - counts.FromFiles, clipFilesDiscovered );
        }

        return Common::MakeSuccess( counts );
    }
} // namespace Desert::Animation

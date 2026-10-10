#include "AnimationAsset.hpp"

#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    AnimationAsset::AnimationAsset( const Common::Filepath& filepath ) : AssetBase( filepath, GetTypeID() )
    {
        // THE CLIP'S IDENTITY IS ITS HEADER GUID (ANIM 4, T7e), adopted HERE for SkeletonAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
    }

    AnimationAsset::AnimationAsset( const AssetMetadata& identity, ReloadTwinTag )
         : AssetBase( identity.Filepath, GetTypeID() )
    {
        m_Metadata = identity;
        // A HANDLE OF ITS OWN: AsyncAssetLoader joins requests by handle (one asset object per handle), and
        // the twin is a second object for this file - under the live asset's handle a request for the live
        // one would wait on the twin's read and be told its own, unread, object had loaded. The load reads
        // the file by path, so the handle names only this request (and is kept out of AssetPathIndex).
        MintUnindexedHandle();
    }

    std::shared_ptr<AssetBase> AnimationAsset::MakeReloadTarget() const
    {
        // A clip generated in memory has no file to read again (IsReloadableFromFile).
        if ( !IsReloadableFromFile() )
            return nullptr;
        return std::make_shared<AnimationAsset>( m_Metadata, ReloadTwinTag{} );
    }

    Common::BoolResultStr AnimationAsset::AdoptReloaded( AssetBase& twin )
    {
        auto* read = dynamic_cast<AnimationAsset*>( &twin );
        if ( read == nullptr || !read->m_HasClip )
            return Common::MakeFormattedError<bool>( "'{}': the reloaded twin holds no clip",
                                                     m_Metadata.Filepath.string() );
        // LoadFromFile's commit, moved here from the worker: the clip whole, then a new generation of the
        // track list, so an Animator's per-track cache notices the replacement.
        m_Clip                   = std::move( read->m_Clip );
        m_Clip.Sequence.Revision = ++m_TrackRevision;
        m_HasClip                = true;
        read->m_HasClip          = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr AnimationAsset::LoadFromFile()
    {
        // The old path of a moved asset reads the file where it now lives, through the registry - the same
        // file the constructor took the identity from (ReadTextAssetIdentity).
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        // Through the VFS first, so a packaged build reads the clip out of its .dpak like every other asset,
        // then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else
        {
            auto raw = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !raw )
                return Common::MakeError( raw.GetError() );
            text = raw.ExtractValue();
        }

        const auto dataReflected = Serialization::ReadAnimationJson( text );
        if ( !dataReflected )
            return Common::MakeFormattedError<bool>( "'{}': {}", file.string(), dataReflected.GetError() );

        // The channel list -> clip step is a pure function so its refusals can be tested without an asset
        // system; a clip that cannot bind is an error here, not an empty successful load. Nothing is written
        // into this asset until it succeeds, so a failed reload leaves the previous clip untouched rather
        // than half-replaced.
        auto built = Serialization::BuildClipFromAssetData( dataReflected.GetValue() );
        if ( !built )
        {
            return Common::MakeFormattedError<bool>( "'{}': {}", file.string(), built.GetError() );
        }

        m_Clip = built.ExtractValue();
        // A NEW GENERATION OF THE TRACK LIST. Stamped here rather than by the builder: the builder makes a
        // fresh clip that knows nothing of the one it is about to replace, and it is the REPLACEMENT that
        // any cache downstream has to notice.
        m_Clip.Sequence.Revision = ++m_TrackRevision;
        m_HasClip            = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr AnimationAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;` — a no-op over the largest allocation in the animation system: every
        // bone track holds three keyframe vectors and the clip holds a vector of them.
        if ( !IsReloadableFromFile() )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' holds a clip that was generated in memory (SetInMemoryClip), not read from a file. "
                 "Releasing it would destroy the only copy there is, and nothing could load it back. The "
                 "asset stays resident.",
                 m_Metadata.Filepath.string() );
        }

        // The sequence goes whole (bindings, tracks, keys); its Revision still moves, so a cache keyed on it
        // cannot mistake the reload's sequence for the one this asset handed out.
        m_Clip.Sequence          = Animation::AnimationClip::MakeClipSequence();
        m_Clip.Sequence.Revision = ++m_TrackRevision;
        m_Clip.AnimationName.clear();
        // The skeleton reference goes with the payload: an unloaded clip names no skeleton until it is read.
        m_Clip.Skeleton = {};
        m_HasClip       = false;
        return BOOLSUCCESS;
    }

    Common::Content::AssetGuid AnimationAsset::GetSkeleton() const
    {
        return m_Clip.Skeleton;
    }

    void AnimationAsset::SetSkeleton( const Common::Content::AssetGuid skeleton )
    {
        m_Clip.Skeleton = skeleton;
    }

} // namespace Desert::Assets
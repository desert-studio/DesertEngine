#include "AnimationAsset.hpp"

#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    AnimationAsset::AnimationAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
        // THE CLIP'S IDENTITY IS ITS HEADER GUID (ANIM 4, T7e), adopted HERE for SkeletonAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        const Common::Content::AssetGuid guid = ReadTextHeaderGuid( m_Metadata.Filepath );
        if ( !guid.IsNull() )
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr AnimationAsset::LoadFromFile()
    {
        // Through the VFS first, so a packaged build reads the clip out of its .dpak like every other asset,
        // then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else
        {
            auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
            if ( !raw )
                return Common::MakeError( raw.GetError() );
            text = raw.ExtractValue();
        }

        const auto dataReflected = Serialization::ReadAnimationJson( text );
        if ( !dataReflected )
            return Common::MakeFormattedError<bool>( "'{}': {}", m_Metadata.Filepath.string(),
                                                     dataReflected.GetError() );

        // The channel list -> clip step is a pure function so its refusals can be tested without an asset
        // system; a clip that cannot bind is an error here, not an empty successful load. Nothing is written
        // into this asset until it succeeds, so a failed reload leaves the previous clip untouched rather
        // than half-replaced.
        auto built = Serialization::BuildClipFromAssetData( dataReflected.GetValue() );
        if ( !built )
        {
            return Common::MakeFormattedError<bool>( "'{}': {}", m_Metadata.Filepath.string(), built.GetError() );
        }

        m_Clip = built.ExtractValue();
        // A NEW GENERATION OF THE TRACK LIST. Stamped here rather than by the builder: the builder makes a
        // fresh clip that knows nothing of the one it is about to replace, and it is the REPLACEMENT that
        // any cache downstream has to notice.
        m_Clip.TrackRevision = ++m_TrackRevision;
        m_SkeletonSignature  = m_Clip.SkeletonSignature;
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

        m_Clip.Tracks.clear();
        m_Clip.Tracks.shrink_to_fit();
        m_Clip.TrackRevision = ++m_TrackRevision; // the list this asset handed out no longer exists
        m_Clip.Notifies.clear();
        m_Clip.Notifies.shrink_to_fit();
        m_Clip.AnimationName.clear();
        m_Clip.DurationTicks = Animation::FrameNumber{};
        // The signature is what ResolveDependencies matches a rig on, so an unloaded clip must not keep
        // answering with one — the same reason the skeleton's readiness is now the skeleton itself.
        m_Clip.SkeletonSignature = 0;
        m_SkeletonSignature      = 0;
        m_HasClip                = false;
        return BOOLSUCCESS;
    }

} // namespace Desert::Assets
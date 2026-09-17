#include "AnimationAsset.hpp"

#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    AnimationAsset::AnimationAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
    }

    Common::BoolResultStr AnimationAsset::LoadFromFile()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );

        // DefaultIfMissing: clips cooked before a field existed (e.g. Notifies) still load — the missing
        // field takes its default (empty) instead of failing the whole read.
        const auto dataReflected =
             rfl::json::read<Serialization::AnimationAssetData, rfl::DefaultIfMissing>( raw.GetValue() );
        if ( !dataReflected.has_value() )
        {
            return Common::MakeError( dataReflected.error().what() );
        }

        // The channel list -> clip step is a pure function so its refusals can be tested without an asset
        // system; a clip that cannot bind is an error here, not an empty successful load. Nothing is written
        // into this asset until it succeeds, so a failed reload leaves the previous clip untouched rather
        // than half-replaced.
        auto built = Serialization::BuildClipFromAssetData( dataReflected.value() );
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
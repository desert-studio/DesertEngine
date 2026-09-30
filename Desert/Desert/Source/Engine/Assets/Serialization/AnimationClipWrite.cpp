#include "AnimationClipWrite.hpp"
#include "AnimationClipBuild.hpp"
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // THE SOURCE THE FILE BEING REPLACED NAMES (THM-FIXJ), kept as its GUID is: a save of an imported clip
        // must not cut it from the source its Reimport re-imports. nullopt for a new file or a hand-authored
        // clip; an error when the file there is not a clip this build reads - a save must not guess over it.
        Common::ResultStr<std::optional<ImportSourceInfo>>
        ImportOfFileBeingReplaced( const std::filesystem::path& path )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContentIfExists( path );
            if ( !text )
                return Common::MakeError<std::optional<ImportSourceInfo>>( text.GetError() );
            const auto& contents = text.GetValue();
            if ( !contents.has_value() )
                return Common::MakeSuccess( std::optional<ImportSourceInfo>{} );
            const auto clip = ReadAnimationJson( *contents );
            if ( !clip )
                return Common::MakeError<std::optional<ImportSourceInfo>>(
                     std::format( "'{}' is replaced by a save, but {}", path.string(), clip.GetError() ) );
            return Common::MakeSuccess( clip.GetValue().Import );
        }
    } // namespace

    Common::BoolResultStr SaveClipToFile( const std::filesystem::path& path, const Animation::AnimationClip& clip )
    {
        // A SAVE KEEPS THE CLIP'S IDENTITY: the GUID the file being replaced states, minted only for a new
        // file (ANIM 4, T7e). Sequencer tracks and anim graphs name the clip, and a fresh GUID would orphan
        // them. The header is stamped here, at the writer, so the version it states is this build's.
        auto built = BuildAssetDataFromClip( clip );
        if ( !built )
            return Common::MakeError<bool>( built.GetError() );
        AnimationAssetData data = built.ExtractValue();
        data.Header =
             HeaderKeepingFileGuid( path, Common::Content::ContentKind::Animation, AnimationTextSubsystems() );
        const auto import = ImportOfFileBeingReplaced( path );
        if ( !import )
            return Common::MakeError<bool>( import.GetError() );
        data.Import              = import.GetValue();
        const auto canonicalJson = Common::Content::CanonicalJsonTextOfWriterOutput( WriteAnimationJson( data ) );
        if ( !canonicalJson )
            return Common::MakeError<bool>( canonicalJson.GetError() );
        const std::string& json = canonicalJson.GetValue();

        // The verdict is the primitive's, and it is read before this function returns. See the header
        // for the shape this replaces.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, json ); !written )
            return Common::MakeFormattedError<bool>( "clip '{}' ({} bytes) was not saved to '{}': {}",
                                                     clip.AnimationName, json.size(), path.string(),
                                                     written.GetError() );

        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization

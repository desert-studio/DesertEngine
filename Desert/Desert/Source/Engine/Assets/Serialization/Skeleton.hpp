#pragma once

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    struct SkeletonAssetData
    {
        /// The text asset header (T7e, SKEL 1), FIRST so the registry reads it without parsing the bones: Kind
        /// "Skeleton", the GUID that IS the rig's identity and its handle (SkeletonAsset's constructor), and the
        /// format under `SKEL`. Absent only on data never written - WriteSkeletonJson mints it then.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        // Initialised for the same reason as AnimationAssetData's scalars: this struct IS the .skeleton file,
        // and 0 is the value SkinnedMeshAsset already reads as "no rig claimed".
        uint64_t                                 Signature = 0;
        std::vector<Desert::Animation::BoneInfo> Bones;
        /// SKEL 3 dropped `Import` (source name + hash): a skinned import's freshness is its import record's
        /// SourceHash (ImportRecord.hpp), which nothing on the rig restated.
        /// SKEL 2 (SKEL-TREE, contract Engine/Animation/SkeletonReference.hpp): the skinned mesh the Skeleton
        /// Editor previews this rig on (UE USkeleton::PreviewSkeletalMesh); null = bones only.
        std::optional<AssetGuidRef> PreviewMesh;
        /// SKEL 2: skeletons whose clips play on meshes of THIS one (UE USkeleton::CompatibleSkeletons) - one
        /// direction, not transitive. Every file states the list, empty when there is none.
        std::vector<AssetGuidRef> CompatibleSkeletons;
    };

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> SkeletonTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ kSkeletonSchemaTag, kSkeletonSchemaVersion } };
        return versions;
    }

    /// The .skeleton text. Stamps the header: the GUID `data` carries is kept, a missing one minted.
    [[nodiscard]] inline std::string WriteSkeletonJson( const SkeletonAssetData& data )
    {
        SkeletonAssetData out = data;
        out.Header =
             StampTextHeader( data.Header, Common::Content::ContentKind::Skeleton, SkeletonTextSubsystems() );
        return Common::Json::Write( out );
    }

    /// Refuses a file with no header (generation 0, before T7e) by name, pointing at Tools/SceneMigrator, and a
    /// header of another kind or version; otherwise an error string on bad JSON.
    [[nodiscard]] inline Common::ResultStr<SkeletonAssetData> ReadSkeletonJson( const std::string& text )
    {
        constexpr int current = static_cast<int>( kSkeletonSchemaVersion );
        // Generation 0 stated no version at all, so a file without a header IS version 0.
        if ( auto headed = RefuseTextWithoutHeader( text, current, 0 ); !headed )
            return Common::MakeError<SkeletonAssetData>( std::format( "skeleton {}", headed.GetError() ) );
        auto parsed = Common::Json::Read<SkeletonAssetData>( text );
        if ( !parsed )
            return Common::MakeError<SkeletonAssetData>( std::format( "bad .skeleton: {}", parsed.GetError() ) );
        if ( auto header = CheckStatedHeader( parsed.GetValue().Header, Common::Content::ContentKind::Skeleton,
                                              kSkeletonSchemaTag, current, SkeletonTextSubsystems() );
             !header )
            return Common::MakeError<SkeletonAssetData>( std::format( "skeleton {}", header.GetError() ) );
        return Common::MakeSuccess( parsed.GetValue() );
    }

    /// THE ONE READ OF A .skeleton FILE: through the VFS first, so a packaged build reads the rig out of its
    /// .dpak like every other asset, then off the disk for a loose file the pak does not carry. Both readers of
    /// a rig — SkeletonAsset and the built-in humanoid (ProceduralCharacterSkeleton) — come through here. An
    /// error names the file.
    [[nodiscard]] inline Common::ResultStr<SkeletonAssetData> ReadSkeletonFile( const std::filesystem::path& file )
    {
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else
        {
            auto raw = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !raw )
                return Common::MakeError<SkeletonAssetData>(
                     std::format( "'{}': {}", file.string(), raw.GetError() ) );
            text = raw.ExtractValue();
        }
        auto read = ReadSkeletonJson( text );
        if ( !read )
            return Common::MakeError<SkeletonAssetData>(
                 std::format( "'{}': {}", file.string(), read.GetError() ) );
        return read;
    }
} // namespace Desert::Assets::Serialization

#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace Desert::Editor::ImportedAssetSource
{
    namespace Detail
    {
        // The `Import` a text asset states, read by @p read (ReadAnimationJson / ReadSkeletonJson).
        template <typename Read>
        Common::ResultStr<std::optional<std::filesystem::path>> StatedSource( const std::filesystem::path& asset,
                                                                              Read&&                       read )
        {
            using Source    = std::optional<std::filesystem::path>;
            const auto text = Common::Utils::FileSystem::ReadFileContentIfExists( asset );
            if ( !text )
                return Common::MakeError<Source>( text.GetError() );
            const std::optional<std::string>& content = text.GetValue();
            if ( !content.has_value() )
                return Common::MakeFormattedError<Source>( "'{}' does not exist", asset.string() );
            const auto data = read( content.value() );
            if ( !data )
                return Common::MakeFormattedError<Source>( "'{}': {}", asset.string(), data.GetError() );
            const auto& import = data.GetValue().Import;
            if ( !import.has_value() || import->Source.empty() )
                return Common::MakeSuccess( Source{} );
            return Common::MakeSuccess( Source{ asset.parent_path() / import->Source } );
        }
    } // namespace Detail

    // THE RAW SOURCE A SKINNED IMPORT'S FILE STATES (UE: UAssetImportData on a USkeleton / UAnimSequence /
    // USkeletalMesh) - read from the asset, never inferred from its name: `<stem>_<clip>.anim` has no inverse
    // (`Fox_Extra_Walk.anim` is clip "Extra_Walk" of Fox.glb as readily as clip "Walk" of Fox_Extra.glb).
    //   `.anim`     - the clip's `Import` (ImportManager::SerializeAnimationAsset, THM-FIXJ);
    //   `.skeleton` - the rig's `Import` (ImportManager::SerializeSkeletonAsset);
    //   `.skmesh`   - the `Import` of the rig the same import wrote beside it: both are CookPaths::SkinnedAsset
    //                 of one source, so they share a stem, and the binary mesh header states no source of its own.
    // nullopt: the asset states no source (hand-authored). An error naming the file when it (or the `.skmesh`'s
    // rig) is missing or unreadable, or when @p asset is not a skinned import's file.
    inline Common::ResultStr<std::optional<std::filesystem::path>>
    SkinnedAssetSource( const std::filesystem::path& asset )
    {
        using Source                = std::optional<std::filesystem::path>;
        const std::string extension = asset.extension().string();
        if ( extension == ".anim" )
            return Detail::StatedSource( asset, []( const std::string& text )
                                         { return Assets::Serialization::ReadAnimationJson( text ); } );
        if ( extension == ".skeleton" || extension == ".skmesh" )
        {
            std::filesystem::path rig = asset;
            rig.replace_extension( ".skeleton" );
            return Detail::StatedSource( rig, []( const std::string& text )
                                         { return Assets::Serialization::ReadSkeletonJson( text ); } );
        }
        return Common::MakeFormattedError<Source>(
             "'{}' is not a skinned import's file (.skmesh, .skeleton, .anim)", asset.string() );
    }
} // namespace Desert::Editor::ImportedAssetSource

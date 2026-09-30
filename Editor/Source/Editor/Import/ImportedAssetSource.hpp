#pragma once

#include "CookPaths.hpp"

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::ImportedAssetSource
{
    namespace Detail
    {
        // The `Import` a clip states (AnimationAssetData::Import, THM-FIXJ; UE UAnimSequence::AssetImportData).
        inline Common::ResultStr<std::optional<std::filesystem::path>>
        ClipStatedSource( const std::filesystem::path& clip )
        {
            using Source    = std::optional<std::filesystem::path>;
            const auto text = Common::Utils::FileSystem::ReadFileContentIfExists( clip );
            if ( !text )
                return Common::MakeError<Source>( text.GetError() );
            const std::optional<std::string>& content = text.GetValue();
            if ( !content.has_value() )
                return Common::MakeFormattedError<Source>( "'{}' does not exist", clip.string() );
            const auto data = Assets::Serialization::ReadAnimationJson( content.value() );
            if ( !data )
                return Common::MakeFormattedError<Source>( "'{}': {}", clip.string(), data.GetError() );
            const auto& import = data.GetValue().Import;
            if ( !import.has_value() || import->Source.empty() )
                return Common::MakeSuccess( Source{} );
            return Common::MakeSuccess( Source{ clip.parent_path() / import->Source } );
        }

        // Whether an import of @p kind writes a file of @p extension: the mesh only for a SkinnedMesh import;
        // the rig for a SkinnedMesh import or a Skeleton one (a rig with clips and no mesh).
        inline bool ImportKindWrites( const Common::Content::ContentKind kind, const std::string& extension )
        {
            using Common::Content::ContentKind;
            if ( extension == ".skmesh" )
                return kind == ContentKind::SkinnedMesh;
            return kind == ContentKind::SkinnedMesh || kind == ContentKind::Skeleton;
        }

        // The source whose IMPORT RECORD beside @p asset wrote it (`.skmesh` / `.skeleton`): the record whose
        // source's CookPaths::SkinnedAsset of this extension IS the file and whose Kind writes that file. The
        // file states no source of its own - the binary mesh never did, and SKEL 3 dropped the rig's `Import`
        // (a skeleton is a shared asset many meshes reference by GUID, UE USkeleton with no AssetImportData).
        inline Common::ResultStr<std::optional<std::filesystem::path>>
        RecordedSource( const std::filesystem::path& asset )
        {
            using Source = std::optional<std::filesystem::path>;
            std::error_code ec;
            if ( !std::filesystem::is_regular_file( asset, ec ) )
                return Common::MakeFormattedError<Source>( "'{}' does not exist", asset.string() );
            const std::string                  extension = asset.extension().string();
            std::vector<std::filesystem::path> writers;
            for ( const std::filesystem::path& source : Common::Content::SourcesRecordedIn( asset.parent_path() ) )
            {
                if ( CookPaths::SkinnedAsset( source, extension ).filename() != asset.filename() )
                    continue;
                const auto kind = Assets::Serialization::ReadImportRecordKind( source );
                if ( !kind )
                    return Common::MakeError<Source>( kind.GetError() );
                if ( ImportKindWrites( kind.GetValue(), extension ) )
                    writers.push_back( source );
            }
            if ( writers.empty() )
                return Common::MakeSuccess( Source{} );
            if ( writers.size() > 1 )
                return Common::MakeFormattedError<Source>(
                     "'{}' is written by the imports of both '{}' and '{}': one file, two sources", asset.string(),
                     writers[0].string(), writers[1].string() );
            return Common::MakeSuccess( Source{ writers.front() } );
        }
    } // namespace Detail

    // THE RAW SOURCE A SKINNED IMPORT'S FILE CAME FROM (UE: the AssetImportData Reimport follows) - read, never
    // inferred from a name alone: `<stem>_<clip>.anim` has no inverse (`Fox_Extra_Walk.anim` is clip
    // "Extra_Walk" of Fox.glb as readily as clip "Walk" of Fox_Extra.glb).
    //   `.anim`               - the clip's own `Import` (ImportManager::SerializeAnimationAsset, THM-FIXJ);
    //   `.skmesh`/`.skeleton` - the import record beside it that wrote it (Detail::RecordedSource): a rig is a
    //                           shared asset that states no source (SKEL 3), and the binary mesh never did.
    // nullopt: no import states the file (a hand-authored clip, a rig no record beside it wrote). An error naming
    // the file when it is missing or unreadable, when a record is unreadable or two claim it, or when @p asset is
    // not a skinned import's file.
    inline Common::ResultStr<std::optional<std::filesystem::path>>
    SkinnedAssetSource( const std::filesystem::path& asset )
    {
        using Source                = std::optional<std::filesystem::path>;
        const std::string extension = asset.extension().string();
        if ( extension == ".anim" )
            return Detail::ClipStatedSource( asset );
        if ( extension == ".skeleton" || extension == ".skmesh" )
            return Detail::RecordedSource( asset );
        return Common::MakeFormattedError<Source>(
             "'{}' is not a skinned import's file (.skmesh, .skeleton, .anim)", asset.string() );
    }
} // namespace Desert::Editor::ImportedAssetSource

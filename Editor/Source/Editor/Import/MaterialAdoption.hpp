#pragma once

#include <Common/Core/Constants.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportResult.hpp>

#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

// AN IMPORT ADOPTS THE MATERIAL THAT IS ALREADY THERE (UE: re-import keeps the material asset and its identity).
//
// A mesh's materials are written only if MISSING, so a .demat that exists is content the project owns — edited
// by hand, or lifted by a migrator under a GUID of its own. The importer derives a GUID from the mesh's place
// in the project; when the file already exists under a DIFFERENT one, a mesh that stamped the derived GUID
// into its submeshes names a material no file states, and every dependency of the cooked mesh dangles. So
// the materials are resolved FIRST, and the GUID a submesh carries is the one the file on disk states.
namespace Desert::Editor::MaterialAdoption
{
    // Resources/Assets/Materials/<meshRelativeId>/<materialName>.demat. The name is made filesystem-safe here
    // and nowhere else, so the file the writer checks is the file the adoption reads.
    inline std::filesystem::path MaterialAssetPath( const std::filesystem::path& sourcePath,
                                                    const std::string&           materialName )
    {
        static const std::regex illegal( R"([<>:"/\\|?*\s])" );
        const std::string       safeName = std::regex_replace( materialName, illegal, "_" );
        return CookPaths::MaterialFolder( sourcePath ) /
               ( safeName + std::string( Common::Constants::Extensions::MATERIAL_EXTENSION ) );
    }

    // The GUID the .demat at `path` states; std::nullopt when there is no file. A file that exists but states
    // no readable GUID is an ERROR naming it: adopting nothing would silently re-point the mesh at a GUID the
    // file does not carry, which is the dangling reference this exists to prevent.
    inline Common::ResultStr<std::optional<Common::Content::AssetGuid>>
    ReadExistingMaterialGuid( const std::filesystem::path& path )
    {
        const auto read = Common::Utils::FileSystem::ReadFileContentIfExists( path );
        if ( !read )
            return Common::MakeFormattedError<std::optional<Common::Content::AssetGuid>>(
                 "material '{}' could not be read: {}", path.string(), read.GetError() );
        const auto& content = read.GetValue();
        if ( !content.has_value() )
            return Common::MakeSuccess( std::optional<Common::Content::AssetGuid>{} );

        std::istringstream in( content.value() );
        const auto         object = Common::Content::ReadTextHeaderObject( in );
        if ( !object )
            return Common::MakeFormattedError<std::optional<Common::Content::AssetGuid>>(
                 "material '{}' states no header: {}", path.string(), object.GetError() );
        const auto header = Common::Content::ParseTextHeaderObject( object.GetValue() );
        if ( !header )
            return Common::MakeFormattedError<std::optional<Common::Content::AssetGuid>>(
                 "material '{}' header does not parse: {}", path.string(), header.GetError() );
        const auto guid = Common::Content::AssetGuidFromText( header.GetValue().Guid );
        if ( !guid )
            return Common::MakeFormattedError<std::optional<Common::Content::AssetGuid>>(
                 "material '{}' header GUID refused: {}", path.string(), guid.GetError() );
        if ( guid.GetValue().IsNull() )
            return Common::MakeFormattedError<std::optional<Common::Content::AssetGuid>>(
                 "material '{}' states a null GUID", path.string() );
        return Common::MakeSuccess( std::optional<Common::Content::AssetGuid>( guid.GetValue() ) );
    }

    // Rewrites `result` so every material whose .demat already exists carries that file's GUID, and every
    // submesh that referenced the derived GUID references the adopted one. Runs before anything is written.
    inline Common::BoolResultStr AdoptExistingMaterials( ImportResult&                result,
                                                         const std::filesystem::path& sourcePath )
    {
        for ( auto& material : result.Materials )
        {
            const auto existing = ReadExistingMaterialGuid( MaterialAssetPath( sourcePath, material.Name ) );
            if ( !existing )
                return Common::MakeError<bool>( existing.GetError() );
            const auto& existingGuid = existing.GetValue();
            if ( !existingGuid.has_value() || existingGuid.value() == material.Guid )
                continue;

            const Common::Content::AssetGuid derived = material.Guid;
            material.Guid                            = existingGuid.value();
            if ( result.Mesh )
                for ( auto& submesh : result.Mesh->Submeshes )
                    if ( submesh.MaterialGuid == derived )
                        submesh.MaterialGuid = material.Guid;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::MaterialAdoption

#include <Common/Content/ImportRecord.hpp>

#include <string>

namespace Common::Content
{
    std::optional<std::filesystem::path> MeshSourceBeside( const std::filesystem::path& assetPath )
    {
        for ( const std::string_view ext : kRawMeshSourceExtensions )
        {
            std::filesystem::path candidate = assetPath;
            candidate.replace_extension( ext );
            std::error_code ec;
            if ( std::filesystem::exists( candidate, ec ) )
                return candidate;
        }
        return std::nullopt;
    }

    std::filesystem::path ImportRecordPathFor( const std::filesystem::path& source )
    {
        std::filesystem::path record = source;
        record += std::string( kImportRecordSuffix );
        return record;
    }

    bool IsImportRecord( const std::filesystem::path& file )
    {
        const std::filesystem::path inner = file.stem(); // base.fbx of base.fbx.deimport
        return file.extension() == kImportRecordSuffix && !inner.extension().empty();
    }

    std::filesystem::path MeshAssetOfImportRecord( const std::filesystem::path& record )
    {
        std::filesystem::path asset = record.parent_path() / record.stem(); // base.fbx
        asset.replace_extension( ".stmesh" );
        return asset;
    }

    std::optional<std::filesystem::path> ImportRecordStandingFor( const std::filesystem::path& assetPath )
    {
        std::error_code ec;
        if ( assetPath.empty() || std::filesystem::exists( assetPath, ec ) )
            return std::nullopt;
        const auto source = MeshSourceBeside( assetPath );
        if ( !source )
            return std::nullopt;
        std::filesystem::path record = ImportRecordPathFor( *source );
        if ( !std::filesystem::is_regular_file( record, ec ) )
            return std::nullopt;
        return record;
    }
} // namespace Common::Content

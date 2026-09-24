#include <Engine/World/Landscape/LandscapeTileFiles.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace Desert::World::Landscape
{
    Common::BoolResultStr WriteLandscapeTileFile( const std::filesystem::path& path,
                                                  const LandscapeTileData&     tile )
    {
        if ( tile.Samples().empty() )
            return Common::MakeFormattedError<bool>( "Landscape tile {} has no samples; nothing was written",
                                                     path.generic_string() );

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "could not create the directory {} for a landscape tile: {}",
                                                     path.parent_path().generic_string(), ec.message() );

        const std::vector<unsigned char> blob = EncodeLandscapeTile( tile );
        if ( const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  path, std::as_bytes( std::span( blob.data(), blob.size() ) ) );
             !written )
            return Common::MakeFormattedError<bool>( "could not write landscape tile {}: {}",
                                                     path.generic_string(), written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<LandscapeTileData> ReadLandscapeTileFile( const std::filesystem::path& path )
    {
        auto bytes = Common::Utils::FileSystem::ReadByteFileContent( path );
        if ( !bytes )
            return Common::MakeFormattedError<LandscapeTileData>( "could not read landscape tile {}: {}",
                                                                  path.generic_string(), bytes.GetError() );

        const std::vector<uint8_t> content = bytes.ExtractValue();
        auto decoded = DecodeLandscapeTile( std::span<const unsigned char>( content.data(), content.size() ) );
        if ( !decoded )
            return Common::MakeFormattedError<LandscapeTileData>( "landscape tile {} is not a valid tile: {}",
                                                                  path.generic_string(), decoded.GetError() );
        return decoded;
    }
} // namespace Desert::World::Landscape

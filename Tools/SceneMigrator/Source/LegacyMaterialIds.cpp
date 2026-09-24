#include "LegacyMaterialIds.hpp"

#include "SceneMigration.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

#include <cctype>
#include <regex>
#include <sstream>
#include <system_error>
#include <vector>

namespace Desert::Migration
{
    namespace
    {
        struct RegisterRow
        {
            uint64_t    MaterialId = 0;
            std::string Guid;
        };
        struct RegisterFile
        {
            std::vector<RegisterRow> Ids;
        };

        const std::regex& VersionPattern()
        {
            static const std::regex pattern( R"re("MATL"\s*:\s*(\d+))re" );
            return pattern;
        }

        // The first match of `"<key>": <digits>`, refusing a second: a key stated twice is not a file this
        // step can reason about.
        Common::ResultStr<std::optional<std::smatch>>
        FindNumberMember( std::string_view source, const std::string& text, const std::string& key )
        {
            const std::regex           pattern( "\"" + key + R"re("\s*:\s*(\d+))re" );
            std::optional<std::smatch> found;
            for ( auto it = std::sregex_iterator( text.begin(), text.end(), pattern );
                  it != std::sregex_iterator(); ++it )
            {
                if ( found )
                    return Common::MakeError<std::optional<std::smatch>>(
                         "'" + std::string( source ) + "' states '" + key + "' more than once" );
                found = *it;
            }
            return Common::MakeSuccess( found );
        }

        // `text` without the member `match` spans, and without the one comma that separated it from a
        // neighbour.
        std::string EraseMember( const std::string& text, std::size_t begin, std::size_t end )
        {
            std::size_t before = begin;
            while ( before > 0 && std::isspace( static_cast<unsigned char>( text[before - 1] ) ) )
                --before;
            if ( before > 0 && text[before - 1] == ',' )
                return text.substr( 0, before - 1 ) + text.substr( end );
            std::size_t after = end;
            while ( after < text.size() && std::isspace( static_cast<unsigned char>( text[after] ) ) )
                ++after;
            if ( after < text.size() && text[after] == ',' )
                return text.substr( 0, begin ) + text.substr( after + 1 );
            return text.substr( 0, begin ) + text.substr( end );
        }
    } // namespace

    std::filesystem::path LegacyMaterialIdRegisterPath( const std::filesystem::path& assetsRoot )
    {
        return assetsRoot.lexically_normal().parent_path() / kLegacyMaterialIdRegisterName;
    }

    Common::ResultStr<StatedMaterialIds> ReadStatedMaterialIds( std::string_view source, const std::string& text )
    {
        StatedMaterialIds stated;
        const std::string where = "'" + std::string( source ) + "'";

        std::istringstream in( text );
        if ( const auto header = Common::Content::ReadTextHeaderObject( in ); header )
        {
            const auto parsed = rfl::json::read<Common::Content::TextAssetHeaderSerialized>( header.GetValue() );
            if ( !parsed )
                return Common::MakeError<StatedMaterialIds>( where +
                                                             ": unreadable header: " + parsed.error().what() );
            const auto guid = Common::Content::AssetGuidFromText( parsed.value().Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeError<StatedMaterialIds>( where + ": the header states no GUID" );
            stated.Guid     = guid.GetValue();
            const auto matl = parsed.value().Versions.find( "MATL" );
            if ( matl == parsed.value().Versions.end() )
                return Common::MakeError<StatedMaterialIds>( where + ": the header states no MATL version" );
            stated.Version = matl->second;
        }

        for ( const auto* key : { "MaterialId", "ParentMaterialId" } )
        {
            const auto found = FindNumberMember( source, text, key );
            if ( !found )
                return Common::MakeError<StatedMaterialIds>( found.GetError() );
            if ( !found.GetValue() )
                continue;
            const uint64_t value = std::stoull( ( *found.GetValue() )[1].str() );
            ( std::string_view( key ) == "MaterialId" ? stated.MaterialId : stated.ParentMaterialId ) = value;
        }
        return Common::MakeSuccess( stated );
    }

    Common::BoolResultStr AddLegacyMaterialId( LegacyMaterialIdMap& map, uint64_t id,
                                               const Common::Content::AssetGuid& guid, std::string_view source )
    {
        const auto [at, inserted] = map.emplace( id, guid );
        if ( !inserted && !( at->second == guid ) )
            return Common::MakeError( "MaterialId " + std::to_string( id ) + " is stated by '" +
                                      std::string( source ) + "' as GUID " +
                                      Common::Content::AssetGuidToText( guid ) + " but is already GUID " +
                                      Common::Content::AssetGuidToText( at->second ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::string> WriteLegacyMaterialIdRegister( const LegacyMaterialIdMap& map )
    {
        RegisterFile file;
        file.Ids.reserve( map.size() );
        for ( const auto& [id, guid] : map )
            file.Ids.push_back( { id, Common::Content::AssetGuidToText( guid ) } );
        return Common::Content::CanonicalJsonTextOfWriterOutput( rfl::json::write( file ) );
    }

    Common::ResultStr<LegacyMaterialIdMap> ParseLegacyMaterialIdRegister( std::string_view   source,
                                                                          const std::string& text )
    {
        const auto parsed = rfl::json::read<RegisterFile>( text );
        if ( !parsed )
            return Common::MakeError<LegacyMaterialIdMap>(
                 "'" + std::string( source ) + "' is not a material id register: " + parsed.error().what() );
        LegacyMaterialIdMap map;
        for ( const auto& row : parsed.value().Ids )
        {
            const auto guid = Common::Content::AssetGuidFromText( row.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeError<LegacyMaterialIdMap>( "'" + std::string( source ) + "': MaterialId " +
                                                               std::to_string( row.MaterialId ) +
                                                               " names no GUID ('" + row.Guid + "')" );
            if ( const auto added = AddLegacyMaterialId( map, row.MaterialId, guid.GetValue(), source ); !added )
                return Common::MakeError<LegacyMaterialIdMap>( added.GetError() );
        }
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<LegacyMaterialIdMap> LoadLegacyMaterialIds( const std::filesystem::path& assetsRoot )
    {
        LegacyMaterialIdMap map;
        const auto          registerPath = LegacyMaterialIdRegisterPath( assetsRoot );
        std::error_code     ec;
        if ( std::filesystem::exists( registerPath, ec ) )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( registerPath );
            if ( !text )
                return Common::MakeError<LegacyMaterialIdMap>( text.GetError() );
            auto parsed = ParseLegacyMaterialIdRegister( registerPath.generic_string(), text.GetValue() );
            if ( !parsed )
                return parsed;
            map = std::move( parsed.GetValue() );
        }

        for ( auto it = std::filesystem::recursive_directory_iterator( assetsRoot, ec );
              !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( !it->is_regular_file() || it->path().extension() != ".demat" )
                continue;
            const auto text = Common::Utils::FileSystem::ReadFileContent( it->path() );
            if ( !text )
                return Common::MakeError<LegacyMaterialIdMap>( text.GetError() );
            const auto stated = ReadStatedMaterialIds( it->path().generic_string(), text.GetValue() );
            if ( !stated )
                return Common::MakeError<LegacyMaterialIdMap>( stated.GetError() );
            if ( !stated.GetValue().MaterialId )
                continue;
            const Common::Content::AssetGuid guid =
                 stated.GetValue().Version == 0
                      ? MigrationGuidForPath( it->path().lexically_relative( assetsRoot ) )
                      : stated.GetValue().Guid;
            if ( const auto added =
                      AddLegacyMaterialId( map, *stated.GetValue().MaterialId, guid, it->path().generic_string() );
                 !added )
                return Common::MakeError<LegacyMaterialIdMap>( added.GetError() );
        }
        if ( ec )
            return Common::MakeError<LegacyMaterialIdMap>( "cannot walk '" + assetsRoot.generic_string() +
                                                           "': " + ec.message() );
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<bool> SaveLegacyMaterialIds( const std::filesystem::path& assetsRoot,
                                                   const LegacyMaterialIdMap&   map )
    {
        const auto text = WriteLegacyMaterialIdRegister( map );
        if ( !text )
            return Common::MakeError<bool>( text.GetError() );
        const auto      registerPath = LegacyMaterialIdRegisterPath( assetsRoot );
        std::error_code ec;
        if ( std::filesystem::exists( registerPath, ec ) )
            if ( const auto current = Common::Utils::FileSystem::ReadFileContent( registerPath );
                 current && current.GetValue() == text.GetValue() )
                return Common::MakeSuccess( false );
        const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( registerPath, text.GetValue() );
        if ( !written )
            return Common::MakeError<bool>( written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::string> RaiseMaterialTextToV2( std::string_view source, const std::string& text,
                                                          const LegacyMaterialIdMap& map,
                                                          MaterialV2Report&          report )
    {
        const std::string where  = "'" + std::string( source ) + "'";
        const auto        stated = ReadStatedMaterialIds( source, text );
        if ( !stated )
            return Common::MakeError<std::string>( stated.GetError() );
        if ( stated.GetValue().Version != 1 )
            return Common::MakeError<std::string>( where + " states MATL v" +
                                                   std::to_string( stated.GetValue().Version ) +
                                                   "; the v1 -> v2 step reads v1 only" );

        std::string out = text;

        std::smatch version;
        if ( !std::regex_search( out, version, VersionPattern() ) )
            return Common::MakeError<std::string>( where + ": no MATL version member" );
        out.replace( static_cast<std::size_t>( version.position( 1 ) ),
                     static_cast<std::size_t>( version.length( 1 ) ), "2" );

        if ( stated.GetValue().ParentMaterialId )
        {
            const uint64_t oldParent = *stated.GetValue().ParentMaterialId;
            const auto     parent    = map.find( oldParent );
            if ( parent == map.end() )
                return Common::MakeError<std::string>(
                     where + ": ParentMaterialId " + std::to_string( oldParent ) +
                     " is stated by no material the register or the corpus knows" );
            const std::string guidText = Common::Content::AssetGuidToText( parent->second );

            const std::regex emptyDeps( R"re("Dependencies"\s*:\s*\[\s*\])re" );
            std::smatch      deps;
            if ( !std::regex_search( out, deps, emptyDeps ) )
                return Common::MakeError<std::string>( where + ": the header's Dependencies are not empty" );
            out.replace( static_cast<std::size_t>( deps.position( 0 ) ),
                         static_cast<std::size_t>( deps.length( 0 ) ),
                         "\"Dependencies\": [\"" + guidText + "\"]" );

            const auto member = FindNumberMember( source, out, "ParentMaterialId" );
            if ( !member || !member.GetValue() )
                return Common::MakeError<std::string>( where + ": ParentMaterialId vanished while splicing" );
            const auto& m = *member.GetValue();
            out.replace( static_cast<std::size_t>( m.position( 0 ) ), static_cast<std::size_t>( m.length( 0 ) ),
                         "\"Parent\": \"" + guidText + "\"" );
            report.Parented = true;
        }

        if ( stated.GetValue().MaterialId )
        {
            const auto member = FindNumberMember( source, out, "MaterialId" );
            if ( !member || !member.GetValue() )
                return Common::MakeError<std::string>( where + ": MaterialId vanished while splicing" );
            const auto&       m     = *member.GetValue();
            const std::size_t begin = static_cast<std::size_t>( m.position( 0 ) );
            out                     = EraseMember( out, begin, begin + static_cast<std::size_t>( m.length( 0 ) ) );
            report.DroppedId        = true;
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Migration

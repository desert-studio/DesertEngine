#include "LegacyMaterialIds.hpp"

#include "SceneMigration.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
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
    Common::ResultStr<std::string> UpgradeMeshBytesToV3( const std::string_view            source,
                                                         const std::string_view            bytes,
                                                         const Common::Content::AssetGuid& meshGuid,
                                                         const LegacyMaterialIdMap&        map )
    {
        namespace Content = Common::Content;
        // The pre-v3 layout this function is the only reader of: a 24-byte table row per section, sections
        // 1..9 in v1 and 1..10 (PolyGroups appended) in v2, the submesh section (id 4) in 128-byte rows
        // whose last 8 bytes are the material number. v3 moves only those bytes and the prefix.
        constexpr std::size_t kRow = 24, kRowOld = 128, kRowNew = 136, kShared = 120;
        constexpr uint32_t    kSubmeshId = 4, kPolyGroupsId = 10, kSectionsV3 = 10;
        const auto            Fail = [&]( const std::string& why )
        { return Common::MakeFormattedError<std::string>( "'{}' {}", std::string( source ), why ); };

        Content::MeshBinaryFileHeader header{};
        if ( bytes.size() < sizeof( header ) )
            return Fail( "is " + std::to_string( bytes.size() ) + " bytes, shorter than a mesh header" );
        std::memcpy( &header, bytes.data(), sizeof( header ) );
        if ( std::memcmp( header.Magic, Content::kMeshBinaryMagic, sizeof( header.Magic ) ) != 0 ||
             header.ByteOrder != Content::kMeshBinaryByteOrderTag )
            return Fail( "is not a cooked mesh of this host's byte order" );
        if ( header.Version != 1 && header.Version != 2 )
            return Fail( "is mesh version " + std::to_string( header.Version ) + "; only 1 and 2 are raised" );
        if ( header.FileSize != bytes.size() )
            return Fail( "declares " + std::to_string( header.FileSize ) + " bytes and " +
                         std::to_string( bytes.size() ) + " are present" );
        const uint32_t sections = header.Version == 1 ? kSectionsV3 - 1 : kSectionsV3;
        if ( header.SectionCount != sections )
            return Fail( "declares " + std::to_string( header.SectionCount ) + " sections, version " +
                         std::to_string( header.Version ) + " has " + std::to_string( sections ) );
        if ( meshGuid.IsNull() )
            return Fail( "cannot be given the null GUID" );
        if ( bytes.size() < sizeof( header ) + sections * kRow )
            return Fail( "is shorter than its own section table" );

        struct Section
        {
            uint32_t    Id = 0, ElementSize = 0;
            uint64_t    Count = 0;
            std::string Bytes;
        };
        std::vector<Section> table;
        for ( uint32_t i = 0; i < sections; ++i )
        {
            const char* row = bytes.data() + sizeof( header ) + i * kRow;
            Section     section;
            uint64_t    offset = 0;
            std::memcpy( &section.Id, row, 4 );
            std::memcpy( &section.ElementSize, row + 4, 4 );
            std::memcpy( &offset, row + 8, 8 );
            std::memcpy( &section.Count, row + 16, 8 );
            if ( section.Id != i + 1 || section.ElementSize == 0 || offset > bytes.size() ||
                 section.Count > ( bytes.size() - offset ) / section.ElementSize )
                return Fail( "section table row " + std::to_string( i ) + " does not describe this file" );
            section.Bytes.assign( bytes.data() + offset, section.Count * section.ElementSize );
            if ( section.Id == kSubmeshId )
            {
                if ( section.ElementSize != kRowOld )
                    return Fail( "has " + std::to_string( section.ElementSize ) + "-byte submesh rows, version " +
                                 std::to_string( header.Version ) + " writes " + std::to_string( kRowOld ) );
                std::string rows;
                for ( uint64_t s = 0; s < section.Count; ++s )
                {
                    const char* old    = section.Bytes.data() + s * kRowOld;
                    uint64_t    number = 0;
                    std::memcpy( &number, old + kShared, 8 );
                    Content::AssetGuid guid; // number 0 = no material = the null GUID
                    if ( number != 0 )
                    {
                        const auto found = map.find( number );
                        if ( found == map.end() )
                            return Fail( "submesh " + std::to_string( s ) + " names material number " +
                                         std::to_string( number ) +
                                         ", which the legacy material register does not know" );
                        guid = found->second;
                    }
                    rows.append( old, kShared );
                    rows.append( reinterpret_cast<const char*>( &guid.Hi ), 8 );
                    rows.append( reinterpret_cast<const char*>( &guid.Lo ), 8 );
                }
                section.Bytes       = std::move( rows );
                section.ElementSize = kRowNew;
            }
            table.push_back( std::move( section ) );
        }
        if ( header.Version == 1 )
            table.push_back( Section{ kPolyGroupsId, static_cast<uint32_t>( sizeof( int32_t ) ), 0, {} } );

        // Laid out as EncodeMeshBinary lays out v3: the table after the 80-byte prefix, each section at the
        // next 8-byte boundary, zero padding, the file ending on a boundary.
        std::string tableBytes, body;
        uint64_t    at = Content::kMeshBinaryPrefixV3 + kSectionsV3 * kRow;
        for ( const Section& section : table )
        {
            uint64_t offset = at;
            tableBytes.append( reinterpret_cast<const char*>( &section.Id ), 4 );
            tableBytes.append( reinterpret_cast<const char*>( &section.ElementSize ), 4 );
            tableBytes.append( reinterpret_cast<const char*>( &offset ), 8 );
            tableBytes.append( reinterpret_cast<const char*>( &section.Count ), 8 );
            body += section.Bytes;
            at += section.Bytes.size();
            const uint64_t aligned = ( at + 7u ) & ~static_cast<uint64_t>( 7u );
            body.append( static_cast<std::size_t>( aligned - at ), '\0' );
            at = aligned;
        }
        // A file cooked before the header box existed states none, and every v3 row the gather reads must
        // state one: it is the union of the submesh boxes, which the rows carry at bytes 96..120.
        if ( ( header.Flags & Content::kMeshFlagHasBounds ) == 0 )
        {
            const Section& submeshes = table[kSubmeshId - 1];
            if ( submeshes.Count > 0 )
            {
                float lo[3], hi[3];
                for ( uint64_t s = 0; s < submeshes.Count; ++s )
                {
                    float rowLo[3], rowHi[3];
                    std::memcpy( rowLo, submeshes.Bytes.data() + s * kRowNew + 96, sizeof( rowLo ) );
                    std::memcpy( rowHi, submeshes.Bytes.data() + s * kRowNew + 108, sizeof( rowHi ) );
                    for ( int k = 0; k < 3; ++k )
                    {
                        lo[k] = s == 0 ? rowLo[k] : std::min( lo[k], rowLo[k] );
                        hi[k] = s == 0 ? rowHi[k] : std::max( hi[k], rowHi[k] );
                    }
                }
                Content::StateMeshBounds( header, Common::Math::AABB{ glm::vec3( lo[0], lo[1], lo[2] ),
                                                                      glm::vec3( hi[0], hi[1], hi[2] ) } );
            }
        }
        header.Version      = Content::kMeshBinaryVersion;
        header.SectionCount = kSectionsV3;
        header.FileSize     = at;
        std::string out( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.append( reinterpret_cast<const char*>( &meshGuid.Hi ), 8 );
        out.append( reinterpret_cast<const char*>( &meshGuid.Lo ), 8 );
        return Common::MakeSuccess( out + tableBytes + body );
    }
    namespace
    {
        // The header alone of a text asset (`.decloudtype`): the payload is not this step's business.
        struct TextHeaderProbe
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        };

        const char* KindWord( LegacyAssetKind kind )
        {
            switch ( kind )
            {
                case LegacyAssetKind::Texture:
                    return "texture";
                case LegacyAssetKind::CloudType:
                    return "cloud type";
                case LegacyAssetKind::CloudLayout:
                    return "cloud layout";
                case LegacyAssetKind::Shader:
                    return "shader";
            }
            return "?";
        }

        // The GUID text a `.detex` / `.dclayout` envelope header states. RecordOnly: this step reads the
        // identity, not the body, so the subsystem versions are not its to judge.
        Common::ResultStr<std::string> EnvelopeGuidText( const std::filesystem::path& file )
        {
            namespace CC = Common::Content;
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return Common::MakeError<std::string>( "'" + file.generic_string() + "' cannot be opened" );
            const auto header = CC::ReadEnvelopeHeader( in, CC::AssetHeaderReadContext{ {}, true } );
            if ( !header )
                return Common::MakeError<std::string>( "'" + file.generic_string() + "': " + header.GetError() );
            if ( header.GetValue().Asset.Guid.IsNull() )
                return Common::MakeError<std::string>( "'" + file.generic_string() + "' states a null GUID" );
            return Common::MakeSuccess( CC::AssetGuidToText( header.GetValue().Asset.Guid ) );
        }

        Common::ResultStr<std::string> TextHeaderGuidText( const std::filesystem::path& file )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeError<std::string>( text.GetError() );
            const auto probe = rfl::json::read<TextHeaderProbe>( text.GetValue() );
            if ( !probe || !probe.value().Header )
                return Common::MakeError<std::string>( "'" + file.generic_string() + "' states no text header" );
            const auto guid = Common::Content::AssetGuidFromText( probe.value().Header->Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeError<std::string>( "'" + file.generic_string() + "' states GUID '" +
                                                       probe.value().Header->Guid + "', which is not one" );
            return Common::MakeSuccess( Common::Content::AssetGuidToText( guid.GetValue() ) );
        }

        // Adds every file below `root` whose extension `kindOf` names, under FromKey("<tag>:<prefix><relative>").
        template <typename TKindOf>
        Common::BoolResultStr AddAssetsBelow( LegacyAssetRefMap& map, const std::filesystem::path& root,
                                              std::string_view tag, std::string_view prefix, TKindOf&& kindOf )
        {
            std::error_code ec;
            for ( auto it = std::filesystem::recursive_directory_iterator( root, ec );
                  !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( !it->is_regular_file() )
                    continue;
                const std::optional<LegacyAssetKind> kind = kindOf( it->path().extension().string() );
                if ( !kind )
                    continue;
                const std::string key = std::string( tag ) + ':' + std::string( prefix ) +
                                        it->path().lexically_relative( root ).generic_string();
                LegacyAssetRef ref{ *kind, {}, key };
                if ( *kind != LegacyAssetKind::Shader )
                {
                    auto guid = *kind == LegacyAssetKind::CloudType ? TextHeaderGuidText( it->path() )
                                                                    : EnvelopeGuidText( it->path() );
                    if ( !guid )
                        return Common::MakeError<bool>( guid.GetError() );
                    ref.Guid = std::move( guid.GetValue() );
                }
                // The numbers a MATL 2 slot could hold for this file: its path-derived handle, and for a texture
                // also the handle its import stamped into the GUID's high half (TextureSourceAsset: the source
                // image's path-derived number, kept as Guid.Hi so references made before the .detex existed
                // still meet it).
                std::vector<uint64_t> numbers{ static_cast<uint64_t>( Common::AssetHandle::FromKey( key ) ) };
                if ( *kind == LegacyAssetKind::Texture )
                    if ( const auto guid = Common::Content::AssetGuidFromText( ref.Guid ); guid )
                        numbers.push_back( guid.GetValue().Hi );
                for ( const uint64_t number : numbers )
                {
                    const auto [at, added] = map.emplace( number, ref );
                    if ( !added && at->second.Path != key )
                        return Common::MakeError<bool>( "asset number " + std::to_string( number ) +
                                                        " is reached by both '" + at->second.Path + "' and '" +
                                                        key + "', so a material naming it cannot be translated" );
                }
            }
            if ( ec )
                return Common::MakeError<bool>( "cannot walk '" + root.generic_string() + "': " + ec.message() );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<LegacyAssetRefMap> LoadLegacyAssetRefs( const std::filesystem::path& assetsRoot )
    {
        LegacyAssetRefMap map;
        const auto        assetKind = []( const std::string& ext ) -> std::optional<LegacyAssetKind>
        {
            if ( ext == ".detex" )
                return LegacyAssetKind::Texture;
            if ( ext == ".decloudtype" )
                return LegacyAssetKind::CloudType;
            if ( ext == ".dclayout" )
                return LegacyAssetKind::CloudLayout;
            return std::nullopt;
        };
        if ( const auto ok = AddAssetsBelow( map, assetsRoot, "assets", "", assetKind ); !ok )
            return Common::MakeError<LegacyAssetRefMap>( ok.GetError() );

        // The shaders sit beside the assets root under the engine's resource root, which is how the editor's
        // FromCookedPath keyed them ("engine:Shaders/..."). Absent in a bare content root, which is legal.
        const std::filesystem::path shaders = assetsRoot.parent_path() / "Shaders";
        std::error_code             ec;
        if ( std::filesystem::is_directory( shaders, ec ) )
        {
            const auto shaderKind = []( const std::string& ext ) -> std::optional<LegacyAssetKind>
            {
                if ( ext == ".shader" )
                    return LegacyAssetKind::Shader;
                return std::nullopt;
            };
            if ( const auto ok = AddAssetsBelow( map, shaders, "engine", "Shaders/", shaderKind ); !ok )
                return Common::MakeError<LegacyAssetRefMap>( ok.GetError() );
        }
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<Assets::MaterialData>
    RaiseMaterialToV3( std::string_view source, const MaterialDataV2& material, const LegacyAssetRefMap& refs )
    {
        const auto fail = [&source]( const std::string& why )
        { return Common::MakeError<Assets::MaterialData>( "'" + std::string( source ) + "': " + why ); };
        if ( !material.Header )
            return fail( "states no header, so it is not a MATL 2 file" );

        Assets::MaterialData out;
        out.Header     = material.Header;
        out.ShaderName = material.ShaderName;
        out.Parent     = material.Parent;
        out.Params.reserve( material.Params.size() );
        for ( const auto& param : material.Params )
            out.Params.push_back( { param.Name, param.Value } );

        // THE SLOT NAMES THAT ARE NOT SAMPLERS, stated once: CloudRaymarch.shader's asset inputs.
        constexpr std::array<std::string_view, 6> kCloudSlots = { "CloudType1", "CloudType2",    "CloudType3",
                                                                  "CloudType4", "LayoutPattern", "LayoutMask" };
        constexpr std::string_view                kShaderSlot = "Medium";

        for ( const auto& slot : material.Textures )
        {
            const bool cloud  = std::find( kCloudSlots.begin(), kCloudSlots.end(),
                                           std::string_view( slot.Name ) ) != kCloudSlots.end();
            const bool shader = slot.Name == kShaderSlot;
            if ( slot.TextureHandle == 0 )
            {
                if ( shader )
                    out.ShaderRefs.push_back( { slot.Name, {} } );
                else
                    ( cloud ? out.CloudAssets : out.Textures ).push_back( { slot.Name, {}, {} } );
                continue;
            }
            const auto found = refs.find( slot.TextureHandle );
            if ( found == refs.end() )
                return fail(
                     "slot '" + slot.Name + "' names asset number " + std::to_string( slot.TextureHandle ) +
                     ", which no .detex, .decloudtype, .dclayout or .shader under the content root reaches "
                     "- the file it named is gone or was renamed; the material is left untouched" );
            const LegacyAssetRef& ref = found->second;
            const bool            fits =
                 shader ? ref.Kind == LegacyAssetKind::Shader
                            : cloud ? ( slot.Name.rfind( "CloudType", 0 ) == 0 ? ref.Kind == LegacyAssetKind::CloudType
                                                                               : ref.Kind == LegacyAssetKind::CloudLayout )
                                    : ref.Kind == LegacyAssetKind::Texture;
            if ( !fits )
                return fail( "slot '" + slot.Name + "' names asset number " +
                             std::to_string( slot.TextureHandle ) + ", which is the " + KindWord( ref.Kind ) +
                             " '" + ref.Path + "' - not an asset this slot takes" );
            if ( shader )
                out.ShaderRefs.push_back( { slot.Name, ref.Path } );
            else
                ( cloud ? out.CloudAssets : out.Textures ).push_back( { slot.Name, ref.Guid, ref.Path } );
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Migration

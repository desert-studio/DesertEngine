#include "ShaderAssetHeader.hpp"

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <optional>
#include <sstream>

namespace Common::Content
{
    namespace
    {
        // The header line is one short object; a first line longer than this is a file that is not headed,
        // and reading on would be reading the shader's body.
        constexpr std::size_t kMaxHeaderLineBytes = std::size_t{ 64 } * 1024;

        static_assert( kShaderHeaderPrefix.size() <= ASSET_HEADER_SNIFF_BYTES,
                       "the format claims a file by its prefix, so the prefix must fit in the sniffed bytes" );

        class ShaderCommentHeaderFormatImpl final : public IAssetHeaderFormat
        {
        public:
            [[nodiscard]] std::string_view Name() const override
            {
                return "shader comment header";
            }

            [[nodiscard]] bool Recognises( std::span<const std::byte> leading ) const override
            {
                return leading.size() >= kShaderHeaderPrefix.size() &&
                       std::memcmp( leading.data(), kShaderHeaderPrefix.data(), kShaderHeaderPrefix.size() ) == 0;
            }

            ResultStr<AssetHeader> ReadHeader( std::istream&                 in,
                                               const AssetHeaderReadContext& context ) const override
            {
                auto object = ReadShaderHeaderObject( in );
                if ( !object )
                    return MakeError<AssetHeader>( object.GetError() );
                auto parsed = ParseTextHeaderObject( object.GetValue() );
                if ( !parsed )
                    return MakeError<AssetHeader>( parsed.GetError() );
                return TextHeaderToAssetHeader( parsed.GetValue(), context );
            }
        };
    } // namespace

    std::string WriteShaderHeaderLine( const TextAssetHeaderSerialized& header )
    {
        return std::string( kShaderHeaderPrefix ) + Json::Write( header ) + "\n";
    }

    ResultStr<std::string> ReadShaderHeaderObject( std::istream& in )
    {
        std::string line;
        char        c = 0;
        while ( in.get( c ) && c != '\n' )
        {
            line.push_back( c );
            if ( line.size() > kMaxHeaderLineBytes )
                return MakeFormattedError<std::string>( "shader header: the first line runs past {} bytes",
                                                        kMaxHeaderLineBytes );
        }
        // A checkout that converts line endings leaves a carriage return before the newline.
        if ( !line.empty() && line.back() == '\r' )
            line.pop_back();
        if ( !line.starts_with( kShaderHeaderPrefix ) )
            return MakeFormattedError<std::string>( "shader header: the first line does not open with '{}'",
                                                    kShaderHeaderPrefix );
        return MakeSuccess( line.substr( kShaderHeaderPrefix.size() ) );
    }

    ResultStr<TextAssetHeaderSerialized> ReadShaderHeader( std::string_view source )
    {
        std::istringstream in{
             std::string( source.substr( 0, std::min( source.size(), kMaxHeaderLineBytes + 2 ) ) ) };
        auto object = ReadShaderHeaderObject( in );
        if ( !object )
            return MakeError<TextAssetHeaderSerialized>( object.GetError() );
        return ParseTextHeaderObject( object.GetValue() );
    }

    namespace
    {
        std::string_view TrimImport( std::string_view v )
        {
            const std::size_t first = v.find_first_not_of( " \t\r" );
            if ( first == std::string_view::npos )
                return {};
            const std::size_t last = v.find_last_not_of( " \t\r" );
            return v.substr( first, last - first + 1 );
        }

        bool IsImportIdent( std::string_view v )
        {
            return !v.empty() &&
                   std::all_of( v.begin(), v.end(), []( char ch )
                                { return std::isalnum( static_cast<unsigned char>( ch ) ) || ch == '_'; } );
        }

        // `"<source>.<name>"` -> the key; the rest of the line is left in `rest`.
        ResultStr<std::string> ReadImportKey( std::string_view& rest )
        {
            rest = TrimImport( rest );
            const std::size_t close =
                 rest.size() > 1 && rest[0] == '"' ? rest.find( '"', 1 ) : std::string_view::npos;
            if ( close == std::string_view::npos )
                return MakeFormattedError<std::string>( "Import: expected a quoted source key, found '{}'", rest );
            const std::string_view key = rest.substr( 1, close - 1 );
            const std::size_t      dot = key.find( '.' );
            if ( dot == std::string_view::npos || dot == 0 || dot + 1 == key.size() )
                return MakeFormattedError<std::string>(
                     "Import: source key '{}' is not '<source>.<name>' (e.g. \"gltf.baseColorTexture\")", key );
            rest = TrimImport( rest.substr( close + 1 ) );
            return MakeSuccess( std::string( key ) );
        }
    } // namespace

    ResultStr<ShaderImportLine> ParseShaderImportLine( std::string_view line )
    {
        std::string_view rest = TrimImport( line.substr( 0, line.find( "//" ) ) ); // a key never holds "//"
        if ( rest.ends_with( ';' ) )
            rest = TrimImport( rest.substr( 0, rest.size() - 1 ) );
        ShaderImportLine out;
        if ( rest.starts_with( "Requires" ) )
        {
            rest           = rest.substr( std::strlen( "Requires" ) );
            out.IsRequires = true;
        }
        auto key = ReadImportKey( rest );
        if ( !key )
            return MakeError<ShaderImportLine>( key.GetError() );
        out.Row.SourceKey = key.GetValue();
        if ( out.IsRequires )
        {
            if ( !rest.empty() )
                return MakeFormattedError<ShaderImportLine>( "Import: 'Requires \"{}\"' is followed by '{}'",
                                                             out.Row.SourceKey, rest );
            return MakeSuccess( std::move( out ) );
        }
        if ( !rest.starts_with( "->" ) )
            return MakeFormattedError<ShaderImportLine>( "Import: expected '->' after \"{}\", found '{}'",
                                                         out.Row.SourceKey, rest );
        rest                  = TrimImport( rest.substr( 2 ) );
        const std::size_t dot = rest.find( '.' );
        out.Row.Property      = std::string( rest.substr( 0, dot ) );
        if ( dot != std::string_view::npos )
            out.Row.Channels = std::string( rest.substr( dot + 1 ) );
        if ( !IsImportIdent( out.Row.Property ) )
            return MakeFormattedError<ShaderImportLine>(
                 "Import: \"{}\" maps to '{}', which is not a property name", out.Row.SourceKey, rest );
        if ( dot != std::string_view::npos )
        {
            const std::string& ch = out.Row.Channels;
            bool               ok = !ch.empty() && ch.size() <= 4;
            for ( std::size_t i = 0; ok && i < ch.size(); ++i )
                ok = std::strchr( "rgba", ch[i] ) != nullptr && ch.find( ch[i] ) == i;
            if ( !ok )
                return MakeFormattedError<ShaderImportLine>(
                     "Import: \"{}\" -> {}: channels '{}' are not distinct letters of 'rgba'", out.Row.SourceKey,
                     out.Row.Property, ch );
        }
        return MakeSuccess( std::move( out ) );
    }

    ResultStr<ShaderManifest> ReadShaderManifest( std::string_view source )
    {
        ShaderManifest manifest;
        std::size_t    begin    = 0;
        bool           inImport = false; // between `Import {` and its `}`
        while ( begin < source.size() )
        {
            const std::size_t end   = std::min( source.find( '\n', begin ), source.size() );
            std::string_view  line  = source.substr( begin, end - begin );
            begin                   = end + 1;
            const std::size_t first = line.find_first_not_of( " \t\r" );
            if ( first == std::string_view::npos )
                continue;
            line                   = line.substr( first );
            const std::size_t last = line.find_last_not_of( " \t\r" );
            line                   = line.substr( 0, last + 1 );
            const auto word        = [&line]( std::string_view keyword ) -> std::optional<std::string_view>
            {
                if ( !line.starts_with( keyword ) || line.size() <= keyword.size() ||
                     ( line[keyword.size()] != ' ' && line[keyword.size()] != '\t' ) )
                    return std::nullopt;
                const std::size_t at = line.find_first_not_of( " \t", keyword.size() );
                return line.substr( at );
            };
            if ( inImport )
            {
                if ( line.starts_with( "//" ) || line == "{" )
                    continue;
                if ( line == "}" )
                {
                    inImport = false;
                    continue;
                }
                auto row = ParseShaderImportLine( line );
                if ( !row )
                    return MakeError<ShaderManifest>( row.GetError() );
                if ( row.GetValue().IsRequires )
                    manifest.ImportRequires.push_back( row.GetValue().Row.SourceKey );
                else
                    manifest.Import.push_back( row.GetValue().Row );
                continue;
            }
            if ( line == "Import" || line == "Import {" || line == "Import{" )
            {
                if ( manifest.DeclaresImport )
                    return MakeError<ShaderManifest>(
                         "a second 'Import' block: a template has one import contract" );
                manifest.DeclaresImport = true;
                inImport                = true;
                continue;
            }
            if ( const auto role = word( "Role" ) )
            {
                if ( !manifest.Role.empty() )
                    return MakeError<ShaderManifest>( std::format(
                         "a second 'Role {}' after 'Role {}': a template plays one role", *role, manifest.Role ) );
                manifest.Role = std::string( *role );
            }
            else if ( const auto what = word( "Default" ) )
            {
                if ( *what != "Surface" )
                    return MakeError<ShaderManifest>( std::format(
                         "'Default {}': the only default a template declares is 'Default Surface'", *what ) );
                manifest.DefaultSurface = true;
            }
        }
        if ( inImport )
            return MakeError<ShaderManifest>( "the 'Import' block is not closed by '}'" );
        return MakeSuccess( std::move( manifest ) );
    }

    ResultStr<std::string> ReadShaderDeclaredName( std::string_view source )
    {
        constexpr std::string_view keyword = "Shader";
        std::size_t                begin   = 0;
        while ( begin < source.size() )
        {
            const std::size_t end   = std::min( source.find( '\n', begin ), source.size() );
            std::string_view  line  = source.substr( begin, end - begin );
            begin                   = end + 1;
            const std::size_t first = line.find_first_not_of( " \t\r" );
            if ( first == std::string_view::npos )
                continue;
            line = line.substr( first );
            if ( line.starts_with( "//" ) )
                continue;
            const auto refuse = [&line]()
            {
                return MakeError<std::string>( "the first DSL line is '" + std::string( line ) +
                                               "', not 'Shader \"<name>\"'" );
            };
            if ( !line.starts_with( keyword ) )
                return refuse();
            const std::size_t open  = line.find_first_not_of( " \t", keyword.size() );
            const std::size_t close = open == std::string_view::npos ? open : line.find( '"', open + 1 );
            if ( open == keyword.size() || open == std::string_view::npos || line[open] != '"' ||
                 close == std::string_view::npos || close == open + 1 )
                return refuse();
            return MakeSuccess( std::string( line.substr( open + 1, close - open - 1 ) ) );
        }
        return MakeError<std::string>( "no 'Shader \"<name>\"' line: the source holds only comments" );
    }

    const IAssetHeaderFormat& ShaderCommentHeaderFormat()
    {
        static const ShaderCommentHeaderFormatImpl format;
        return format;
    }
} // namespace Common::Content

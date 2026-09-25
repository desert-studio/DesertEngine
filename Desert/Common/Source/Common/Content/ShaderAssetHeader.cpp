#include "ShaderAssetHeader.hpp"

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cstring>
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
        return std::string( kShaderHeaderPrefix ) + rfl::json::write( header ) + "\n";
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

    ResultStr<std::string> ReadShaderDeclaredName( std::string_view source )
    {
        constexpr std::string_view keyword = "Shader";
        std::size_t                begin   = 0;
        while ( begin < source.size() )
        {
            const std::size_t end  = std::min( source.find( '\n', begin ), source.size() );
            std::string_view  line = source.substr( begin, end - begin );
            begin                  = end + 1;
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

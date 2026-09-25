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
        constexpr std::size_t kMaxHeaderLineBytes = 64 * 1024;

        static_assert( kShaderHeaderPrefix.size() <= ASSET_HEADER_SNIFF_BYTES,
                       "the format claims a file by its prefix, so the prefix must fit in the sniffed bytes" );

        class ShaderCommentHeaderFormatImpl final : public IAssetHeaderFormat
        {
        public:
            std::string_view Name() const override
            {
                return "shader comment header";
            }

            bool Recognises( std::span<const std::byte> leading ) const override
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
        std::istringstream in{ std::string( source.substr( 0, std::min( source.size(), kMaxHeaderLineBytes + 2 ) ) ) };
        auto               object = ReadShaderHeaderObject( in );
        if ( !object )
            return MakeError<TextAssetHeaderSerialized>( object.GetError() );
        return ParseTextHeaderObject( object.GetValue() );
    }

    const IAssetHeaderFormat& ShaderCommentHeaderFormat()
    {
        static const ShaderCommentHeaderFormatImpl format;
        return format;
    }
} // namespace Common::Content

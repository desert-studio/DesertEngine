#include "TextAssetHeader.hpp"

#include <rflcpp/rfl/json.hpp>

#include <cctype>
#include <cstdio>
#include <istream>

namespace Common::Content
{
    namespace
    {
        // A header is a handful of short members; a "header" longer than this is a file whose first member
        // never closes, and reading on would be reading the body - the one thing this reader must not do.
        constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

        std::optional<uint32_t> TagFromText( std::string_view text )
        {
            if ( text.size() != 4 )
                return std::nullopt;
            uint32_t tag = 0;
            for ( std::size_t i = 0; i < 4; ++i )
                tag |= static_cast<uint32_t>( static_cast<unsigned char>( text[i] ) ) << ( 8 * i );
            return tag;
        }

        std::string TagToText( uint32_t tag )
        {
            std::string text( 4, ' ' );
            for ( std::size_t i = 0; i < 4; ++i )
                text[i] = static_cast<char>( ( tag >> ( 8 * i ) ) & 0xFF );
            return text;
        }

        bool SkipSpace( std::istream& in, char& c )
        {
            while ( in.get( c ) )
                if ( !std::isspace( static_cast<unsigned char>( c ) ) )
                    return true;
            return false;
        }

        class TextHeaderFormatImpl final : public IAssetHeaderFormat
        {
        public:
            std::string_view Name() const override
            {
                return "text header";
            }

            // `{`, whitespace, `"Header"` - the canonical layout puts it in the first 14 bytes.
            bool Recognises( std::span<const std::byte> leading ) const override
            {
                std::size_t i    = 0;
                const auto  at   = [&]( std::size_t k ) { return static_cast<char>( leading[k] ); };
                const auto  skip = [&]
                {
                    while ( i < leading.size() && std::isspace( static_cast<unsigned char>( at( i ) ) ) )
                        ++i;
                };
                skip();
                if ( i >= leading.size() || at( i ) != '{' )
                    return false;
                ++i;
                skip();
                const std::string quoted = "\"" + std::string( kTextHeaderMember ) + "\"";
                for ( const char expected : quoted )
                {
                    if ( i >= leading.size() || at( i ) != expected )
                        return false;
                    ++i;
                }
                return true;
            }

            ResultStr<AssetHeader> ReadHeader( std::istream&                 in,
                                               const AssetHeaderReadContext& context ) const override
            {
                auto object = ReadTextHeaderObject( in );
                if ( !object )
                    return MakeError<AssetHeader>( object.GetError() );
                auto parsed = ParseTextHeaderObject( object.GetValue() );
                if ( !parsed )
                    return MakeError<AssetHeader>( parsed.GetError() );
                return TextHeaderToAssetHeader( parsed.GetValue(), context );
            }
        };
    } // namespace

    std::string AssetGuidToText( const AssetGuid& guid )
    {
        std::array<char, 33> text{};
        std::snprintf( text.data(), text.size(), "%016llx%016llx", static_cast<unsigned long long>( guid.Hi ),
                       static_cast<unsigned long long>( guid.Lo ) );
        return std::string( text.data(), 32 );
    }

    ResultStr<AssetGuid> AssetGuidFromText( std::string_view text )
    {
        if ( text.size() != 32 )
            return MakeFormattedError<AssetGuid>( "asset GUID '{}' is {} characters, not 32 hex digits", text,
                                                  text.size() );
        AssetGuid guid;
        for ( std::size_t i = 0; i < 32; ++i )
        {
            const char c = text[i];
            uint64_t   digit;
            if ( c >= '0' && c <= '9' )
                digit = static_cast<uint64_t>( c - '0' );
            else if ( c >= 'a' && c <= 'f' )
                digit = static_cast<uint64_t>( c - 'a' + 10 );
            else
                return MakeFormattedError<AssetGuid>( "asset GUID '{}': character {} is not a lower-case hex "
                                                      "digit",
                                                      text, i );
            uint64_t& half = i < 16 ? guid.Hi : guid.Lo;
            half           = ( half << 4 ) | digit;
        }
        return MakeSuccess( guid );
    }

    std::optional<ContentKind> ContentKindNamed( std::string_view name )
    {
        for ( std::size_t i = 0; i < CONTENT_KIND_COUNT; ++i )
            if ( KindName( static_cast<ContentKind>( i ) ) == name )
                return static_cast<ContentKind>( i );
        return std::nullopt;
    }

    TextAssetHeaderSerialized MakeTextHeader( ContentKind kind, const AssetGuid& guid,
                                              std::span<const SubsystemVersion> versions )
    {
        TextAssetHeaderSerialized header;
        header.Kind = std::string( KindName( kind ) );
        header.Guid = AssetGuidToText( guid );
        for ( const SubsystemVersion& v : versions )
            header.Versions[TagToText( v.Tag )] = v.Version;
        return header;
    }

    std::optional<uint32_t> TextHeaderVersion( const TextAssetHeaderSerialized& header, uint32_t tag )
    {
        const auto found = header.Versions.find( TagToText( tag ) );
        if ( found == header.Versions.end() )
            return std::nullopt;
        return found->second;
    }

    ResultStr<AssetHeader> TextHeaderToAssetHeader( const TextAssetHeaderSerialized& header,
                                                    const AssetHeaderReadContext&    context )
    {
        AssetHeader out;
        const auto  kind = ContentKindNamed( header.Kind );
        if ( !kind )
            return MakeFormattedError<AssetHeader>( "text header: unknown kind '{}'", header.Kind );
        out.Kind = *kind;

        auto guid = AssetGuidFromText( header.Guid );
        if ( !guid )
            return MakeFormattedError<AssetHeader>( "text header: {}", guid.GetError() );
        if ( guid.GetValue().IsNull() )
            return MakeError<AssetHeader>( "text header: null GUID - a text asset is given its GUID once, by "
                                           "AssetGuid::Generate() or by the migration, and keeps it" );
        out.Guid = guid.GetValue();

        for ( const auto& [text, version] : header.Versions )
        {
            const auto tag = TagFromText( text );
            if ( !tag )
                return MakeFormattedError<AssetHeader>( "text header: version tag '{}' is not four characters",
                                                        text );
            const auto known = std::find_if( context.KnownSubsystems.begin(), context.KnownSubsystems.end(),
                                             [&]( const SubsystemVersion& k ) { return k.Tag == *tag; } );
            if ( context.RecordOnly )
            {
                out.Subsystems.push_back( SubsystemVersion{ *tag, version } );
                continue;
            }
            if ( known == context.KnownSubsystems.end() )
                return MakeFormattedError<AssetHeader>(
                     "text header: subsystem '{}' (version {}) is unknown to this build", text, version );
            if ( version > known->Version )
                return MakeFormattedError<AssetHeader>(
                     "text header: subsystem '{}' version {} is newer than this build's {}", text, version,
                     known->Version );
            out.Subsystems.push_back( SubsystemVersion{ *tag, version } );
        }

        for ( const std::string& dependency : header.Dependencies )
        {
            auto parsed = AssetGuidFromText( dependency );
            if ( !parsed )
                return MakeFormattedError<AssetHeader>( "text header: dependency {}", parsed.GetError() );
            out.Dependencies.push_back( parsed.GetValue() );
        }
        return MakeSuccess( std::move( out ) );
    }

    ResultStr<std::string> ReadTextHeaderObject( std::istream& in )
    {
        char c = 0;
        if ( !SkipSpace( in, c ) || c != '{' )
            return MakeError<std::string>( "text header: the document does not open with '{'" );
        if ( !SkipSpace( in, c ) || c != '"' )
            return MakeError<std::string>( "text header: the document's first member is not \"Header\"" );
        std::string key;
        while ( in.get( c ) && c != '"' )
            key += c;
        if ( key != kTextHeaderMember )
            return MakeFormattedError<std::string>( "text header: the document's first member is \"{}\", not "
                                                    "\"Header\"",
                                                    key );
        if ( !SkipSpace( in, c ) || c != ':' || !SkipSpace( in, c ) || c != '{' )
            return MakeError<std::string>( "text header: \"Header\" is not an object" );

        // Brace matching, with strings skipped so a '}' inside a value does not close the object.
        std::string object( 1, '{' );
        int         depth    = 1;
        bool        inString = false;
        bool        escaped  = false;
        while ( depth > 0 )
        {
            if ( !in.get( c ) )
                return MakeFormattedError<std::string>( "text header: the file ends inside the header, after {} "
                                                        "bytes of it",
                                                        object.size() );
            if ( object.size() >= kMaxHeaderBytes )
                return MakeFormattedError<std::string>( "text header: the header object does not close within "
                                                        "{} bytes",
                                                        kMaxHeaderBytes );
            object += c;
            if ( inString )
            {
                if ( escaped )
                    escaped = false;
                else if ( c == '\\' )
                    escaped = true;
                else if ( c == '"' )
                    inString = false;
            }
            else if ( c == '"' )
                inString = true;
            else if ( c == '{' )
                ++depth;
            else if ( c == '}' )
                --depth;
        }
        return MakeSuccess( std::move( object ) );
    }

    ResultStr<TextAssetHeaderSerialized> ParseTextHeaderObject( std::string_view object )
    {
        auto parsed = rfl::json::read<TextAssetHeaderSerialized>( std::string( object ) );
        if ( !parsed )
            return MakeFormattedError<TextAssetHeaderSerialized>( "text header: {}", parsed.error().what() );
        return MakeSuccess( std::move( parsed.value() ) );
    }

    const IAssetHeaderFormat& TextHeaderFormat()
    {
        static const TextHeaderFormatImpl format;
        return format;
    }
} // namespace Common::Content

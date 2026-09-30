#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace Desert::Core::ShadingModels
{
    namespace
    {
        // The manifest half of the file: identifiers, numbers, `{`, `}`, `;`, with `//` comments skipped.
        // The body is never tokenized — it starts right after the `}` that closes Inputs.
        struct ManifestCursor
        {
            std::string_view Text;
            std::size_t      Pos = 0;

            void SkipSpaceAndComments()
            {
                while ( Pos < Text.size() )
                {
                    if ( std::isspace( static_cast<unsigned char>( Text[Pos] ) ) != 0 )
                        ++Pos;
                    else if ( Text.substr( Pos, 2 ) == "//" )
                    {
                        const std::size_t end = Text.find( '\n', Pos );
                        Pos                   = end == std::string_view::npos ? Text.size() : end + 1;
                    }
                    else
                        return;
                }
            }

            // The next word (letters, digits, '_'), or empty when the next character is not one.
            std::string_view Word()
            {
                SkipSpaceAndComments();
                const std::size_t start = Pos;
                while ( Pos < Text.size() &&
                        ( std::isalnum( static_cast<unsigned char>( Text[Pos] ) ) != 0 || Text[Pos] == '_' ) )
                    ++Pos;
                return Text.substr( start, Pos - start );
            }

            bool Punct( const char ch )
            {
                SkipSpaceAndComments();
                if ( Pos < Text.size() && Text[Pos] == ch )
                {
                    ++Pos;
                    return true;
                }
                return false;
            }
        };

        using ParseResult = Common::ResultStr<ShadingModelManifest>;

        ParseResult Refuse( const std::filesystem::path& sourcePath, const std::string& what )
        {
            return Common::MakeError<ShadingModelManifest>(
                 std::format( "{}: {}", sourcePath.generic_string(), what ) );
        }

        // `<Keyword> { <line>; ... }` — each line is one or two words; @p lines receives them.
        bool ReadBlock( ManifestCursor& c, const std::string_view keyword, const std::size_t wordsPerLine,
                        std::vector<std::vector<std::string>>& lines, std::string& error )
        {
            if ( c.Word() != keyword )
            {
                error = std::format( "expected the {} block", keyword );
                return false;
            }
            if ( !c.Punct( '{' ) )
            {
                error = std::format( "expected '{{' after {}", keyword );
                return false;
            }
            while ( !c.Punct( '}' ) )
            {
                std::vector<std::string> line;
                for ( std::size_t i = 0; i < wordsPerLine; ++i )
                {
                    const std::string_view word = c.Word();
                    if ( word.empty() )
                    {
                        error = std::format( "{} line {}: expected {} word(s) and ';'", keyword, lines.size() + 1,
                                             wordsPerLine );
                        return false;
                    }
                    line.emplace_back( word );
                }
                if ( !c.Punct( ';' ) )
                {
                    error = std::format( "{} line {}: expected {} word(s) and ';'", keyword, lines.size() + 1,
                                         wordsPerLine );
                    return false;
                }
                lines.push_back( std::move( line ) );
            }
            return true;
        }
    } // namespace

    Common::ResultStr<ShadingModelManifest> ParseShadingModelManifest( const std::string_view       text,
                                                                       const std::filesystem::path& sourcePath )
    {
        ShadingModelManifest manifest;
        manifest.Name       = sourcePath.stem().string();
        manifest.SourcePath = sourcePath;

        ManifestCursor c{ text };

        if ( c.Word() != "Guid" )
            return Refuse( sourcePath, "the manifest must start with 'Guid <decimal 64-bit id>;'" );
        const std::string_view digits = c.Word();
        std::uint64_t          guid   = 0;
        const auto [end, ec]          = std::from_chars( digits.data(), digits.data() + digits.size(), guid );
        if ( digits.empty() || ec != std::errc() || end != digits.data() + digits.size() )
            return Refuse( sourcePath, std::format( "Guid '{}' is not a decimal 64-bit id", digits ) );
        if ( guid == 0 )
            return Refuse( sourcePath, "Guid 0 is the null id" );
        if ( !c.Punct( ';' ) )
            return Refuse( sourcePath, "expected ';' after the Guid" );
        manifest.Guid = Common::UUID( guid );

        std::string                           error;
        std::vector<std::vector<std::string>> payload;
        if ( !ReadBlock( c, "Payload", 2, payload, error ) )
            return Refuse( sourcePath, error );
        if ( payload.size() > kMaxPayloadFloats )
            return Refuse( sourcePath, std::format( "Payload declares {} floats; a model carries at most {}",
                                                    payload.size(), kMaxPayloadFloats ) );
        for ( const auto& line : payload )
        {
            if ( std::ranges::find( kPayloadPins, line[0] ) == kPayloadPins.end() )
                return Refuse( sourcePath, std::format( "Payload pin '{}' is not {} or {}", line[0],
                                                        kPayloadPins[0], kPayloadPins[1] ) );
            if ( std::ranges::find( manifest.Payload, line[0], &PayloadSlot::Pin ) != manifest.Payload.end() )
                return Refuse( sourcePath, std::format( "Payload pin '{}' appears twice", line[0] ) );
            manifest.Payload.push_back( { line[0], line[1] } );
        }

        std::vector<std::vector<std::string>> inputs;
        if ( !ReadBlock( c, "Inputs", 1, inputs, error ) )
            return Refuse( sourcePath, error );
        for ( auto& line : inputs )
            manifest.Inputs.push_back( std::move( line[0] ) );

        manifest.Body = std::string( text.substr( c.Pos ) );
        return Common::MakeSuccess( std::move( manifest ) );
    }
} // namespace Desert::Core::ShadingModels

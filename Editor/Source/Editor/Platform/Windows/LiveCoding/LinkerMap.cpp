#include "LinkerMap.hpp"

#include <charconv>
#include <fstream>
#include <map>
#include <vector>

// The text MSVC link.exe writes for /MAP:
//
//    Preferred load address is 0000000180000000
//
//    Start         Length     Name                   Class
//    0001:00000000 00012345H .text$mn                CODE
//
//     Address         Publics by Value              Rva+Base               Lib:Object
//    0001:00000010       ?Step@SpawnState@VFX@Desert@@QEAAXXZ 0000000180001010 f   unity_0_cxx.cxx.obj
//    0002:00000120       __imp_?Get@JobSystem@Desert@@SAAEAV12@XZ 0000000180085120 DesertEngine:DesertEngine.dll
//
//    entry point at        0001:00001234
//
//    Static symbols
//
//    0001:00000400       ?g_WorldSlot@?A0x344769b6@VFX@Desert@@3_KA 0000000180090400     unity_0_cxx.cxx.obj
namespace Desert::Editor::LiveCoding
{
    namespace
    {
        bool ParseHex( std::string_view text, std::uint64_t& value )
        {
            const auto [end, error] = std::from_chars( text.data(), text.data() + text.size(), value, 16 );
            return error == std::errc{} && end == text.data() + text.size();
        }

        std::vector<std::string_view> Tokens( std::string_view line )
        {
            std::vector<std::string_view> tokens;
            std::size_t                   at = 0;
            while ( at < line.size() )
            {
                while ( at < line.size() && ( line[at] == ' ' || line[at] == '\t' ) )
                {
                    ++at;
                }
                const std::size_t begin = at;
                while ( at < line.size() && line[at] != ' ' && line[at] != '\t' )
                {
                    ++at;
                }
                if ( at > begin )
                {
                    tokens.push_back( line.substr( begin, at - begin ) );
                }
            }
            return tokens;
        }

        // "0001:00000010" -> segment 1, offset 0x10.
        bool ParseAddress( std::string_view text, std::uint32_t& segment, std::uint32_t& offset )
        {
            const std::size_t colon = text.find( ':' );
            std::uint64_t     s = 0, o = 0;
            if ( colon == std::string_view::npos || !ParseHex( text.substr( 0, colon ), s ) ||
                 !ParseHex( text.substr( colon + 1 ), o ) )
            {
                return false;
            }
            segment = std::uint32_t( s );
            offset  = std::uint32_t( o );
            return true;
        }

        bool IsAnonymousHashDigit( char c )
        {
            return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
        }
    } // namespace

    std::string NormaliseAnonymousNamespace( std::string_view decorated )
    {
        std::string out;
        out.reserve( decorated.size() );
        std::size_t at = 0;
        while ( at < decorated.size() )
        {
            const std::size_t hit = decorated.find( "?A0x", at );
            if ( hit == std::string_view::npos )
            {
                out.append( decorated.substr( at ) );
                break;
            }
            out.append( decorated.substr( at, hit - at ) );
            out.append( "?A0x" );
            std::size_t digits = hit + 4;
            while ( digits < decorated.size() && digits - ( hit + 4 ) < 8 &&
                    IsAnonymousHashDigit( decorated[digits] ) )
            {
                ++digits;
            }
            if ( digits - ( hit + 4 ) == 8 && digits < decorated.size() && decorated[digits] == '@' )
            {
                at = digits; // the hash is dropped, its '@' kept
            }
            else
            {
                at = hit + 4; // not an anonymous namespace hash: keep the text as it is
            }
        }
        return out;
    }

    Common::ResultStr<LinkerMap> LinkerMap::Load( const std::filesystem::path& map )
    {
        std::ifstream file( map );
        if ( !file )
        {
            return Common::MakeFormattedError<LinkerMap>( "cannot open the linker map {} (the image was linked "
                                                          "without /MAP)",
                                                          map.generic_string() );
        }

        enum class Part
        {
            Header,
            Sections,
            Publics,
            Statics,
            Done
        };
        struct RawSection
        {
            std::string   Name;
            std::uint32_t Segment;
            std::uint32_t Offset;
        };

        LinkerMap                              result;
        Part                                   part          = Part::Header;
        std::uint64_t                          preferred     = 0;
        bool                                   havePreferred = false;
        std::vector<RawSection>                sections;
        std::map<std::uint32_t, std::uint32_t> segmentRva; // segment -> RVA of its offset 0, from any symbol in it
        std::string                            line;
        while ( std::getline( file, line ) )
        {
            if ( !line.empty() && line.back() == '\r' )
            {
                line.pop_back();
            }
            const std::string_view text( line );
            if ( text.find( "Preferred load address is" ) != std::string_view::npos )
            {
                const std::vector<std::string_view> tokens = Tokens( text );
                havePreferred = !tokens.empty() && ParseHex( tokens.back(), preferred );
                continue;
            }
            if ( text.find( "Start" ) != std::string_view::npos &&
                 text.find( "Length" ) != std::string_view::npos &&
                 text.find( "Class" ) != std::string_view::npos )
            {
                part = Part::Sections;
                continue;
            }
            if ( text.find( "Publics by Value" ) != std::string_view::npos )
            {
                part = Part::Publics;
                continue;
            }
            if ( text.find( "entry point at" ) != std::string_view::npos )
            {
                continue;
            }
            if ( text.find( "Static symbols" ) != std::string_view::npos )
            {
                part = Part::Statics;
                continue;
            }
            if ( text.find( "Exports" ) == 1 || text.find( "Exports" ) == 0 )
            {
                part = Part::Done;
                continue;
            }

            const std::vector<std::string_view> tokens = Tokens( text );
            if ( tokens.empty() )
            {
                continue;
            }
            std::uint32_t segment = 0, offset = 0;
            if ( !ParseAddress( tokens[0], segment, offset ) )
            {
                continue;
            }
            if ( part == Part::Sections && tokens.size() >= 3 )
            {
                sections.push_back( RawSection{ std::string( tokens[2] ), segment, offset } );
                continue;
            }
            if ( ( part != Part::Publics && part != Part::Statics ) || tokens.size() < 4 || !havePreferred )
            {
                continue;
            }
            std::uint64_t va = 0;
            if ( !ParseHex( tokens[2], va ) )
            {
                continue;
            }
            if ( segment == 0 || va < preferred )
            {
                continue; // absolute symbols (__guard_flags, ...) have no address in the image
            }
            const std::uint32_t rva = std::uint32_t( va - preferred );
            segmentRva.try_emplace( segment, rva - offset );

            // After Rva+Base: the flags "f" (function) and "i" (inlined), then Lib:Object, which may hold spaces
            // ("<linker defined>").
            std::size_t objectToken = 3;
            while ( objectToken < tokens.size() && ( tokens[objectToken] == "f" || tokens[objectToken] == "i" ) )
            {
                ++objectToken;
            }
            std::string object;
            if ( objectToken < tokens.size() )
            {
                const char* begin = tokens[objectToken].data();
                const char* end   = tokens.back().data() + tokens.back().size();
                object.assign( begin, end );
            }
            result.m_ByName.emplace( NormaliseAnonymousNamespace( tokens[1] ),
                                     Entry{ rva, std::move( object ), part == Part::Statics } );
        }
        if ( !havePreferred )
        {
            return Common::MakeFormattedError<LinkerMap>(
                 "{} has no \"Preferred load address\": not a link.exe map", map.generic_string() );
        }
        for ( const RawSection& section : sections )
        {
            const auto base = segmentRva.find( section.Segment );
            if ( base != segmentRva.end() )
            {
                result.m_Sections.try_emplace( section.Name, base->second + section.Offset );
            }
        }
        return Common::MakeSuccess( std::move( result ) );
    }

    std::optional<std::uint32_t> LinkerMap::Find( std::string_view decorated, std::string_view object ) const
    {
        const auto [begin, end] = m_ByName.equal_range( NormaliseAnonymousNamespace( decorated ) );
        std::optional<std::uint32_t> staticMatch;
        for ( auto it = begin; it != end; ++it )
        {
            const Entry& entry = it->second;
            if ( !entry.Static )
            {
                return entry.Rva;
            }
            // "unity_0_cxx.cxx.obj", or "Lib:unity_0_cxx.cxx.obj" for an object pulled from a static library.
            const std::string_view column( entry.Object );
            const bool same = column == object || ( column.size() > object.size() && column.ends_with( object ) &&
                                                    column[column.size() - object.size() - 1] == ':' );
            if ( same && !staticMatch )
            {
                staticMatch = entry.Rva;
            }
        }
        return staticMatch;
    }

    std::optional<std::uint32_t> LinkerMap::SectionStart( std::string_view section ) const
    {
        const auto it = m_Sections.find( std::string( section ) );
        if ( it == m_Sections.end() )
        {
            return std::nullopt;
        }
        return it->second;
    }
} // namespace Desert::Editor::LiveCoding

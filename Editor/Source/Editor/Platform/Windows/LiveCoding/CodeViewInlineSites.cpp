#include "CodeViewInlineSites.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

// CodeView C13 in .debug$S (/Z7), microsoft-pdb include/cvinfo.h:
//   section  = u32 CV_SIGNATURE_C13 (4), then subsections { u32 kind, u32 length, data, padding to 4 }
//   0xF1 DEBUG_S_SYMBOLS         symbol records { u16 reclen, u16 kind, ... }
//   0xF2 DEBUG_S_LINES           { u32 offCon (SECREL), u16 segCon (SECTION), u16 flags, u32 cbCon } + file blocks
//                                { u32 fileId, u32 nLines, u32 cbBlock, ... }; fileId = offset into 0xF4
//   0xF3 DEBUG_S_STRINGTABLE     zero-terminated names
//   0xF4 DEBUG_S_FILECHKSMS      { u32 name (into 0xF3), u8 cbChecksum, u8 kind, checksum, padding to 4 }
//   0xF6 DEBUG_S_INLINEELINES    u32 signature (0, or 1 = with extra files), then
//                                { u32 inlinee id, u32 fileId, u32 line [, u32 count, u32 extraFileIds[count]] }
// Procedure records S_GPROC32 / S_LPROC32 (+ _ID): the code offset field is at +32 (SECREL to the function's
// symbol), the name at +39. S_INLINESITE / S_INLINESITE2: the inlinee id at +12.
namespace Desert::Editor::LiveCoding::CodeView
{
    namespace
    {
        constexpr std::uint32_t kSignatureC13      = 4;
        constexpr std::uint32_t kSubsectionSymbols = 0xF1;
        constexpr std::uint32_t kSubsectionLines   = 0xF2;
        constexpr std::uint32_t kSubsectionStrings = 0xF3;
        constexpr std::uint32_t kSubsectionFiles   = 0xF4;
        constexpr std::uint32_t kSubsectionInlinee = 0xF6;
        constexpr std::uint32_t kIgnoreSubsection  = 0x80000000; // DEBUG_S_IGNORE

        constexpr std::uint16_t kSymEnd           = 0x0006; // S_END
        constexpr std::uint16_t kSymThunk32       = 0x1102;
        constexpr std::uint16_t kSymBlock32       = 0x1103;
        constexpr std::uint16_t kSymWith32        = 0x1104;
        constexpr std::uint16_t kSymLProc32       = 0x110F;
        constexpr std::uint16_t kSymGProc32       = 0x1110;
        constexpr std::uint16_t kSymSepCode       = 0x1132;
        constexpr std::uint16_t kSymLProc32Id     = 0x1146;
        constexpr std::uint16_t kSymGProc32Id     = 0x1147;
        constexpr std::uint16_t kSymInlineSite    = 0x114D;
        constexpr std::uint16_t kSymInlineSiteEnd = 0x114E;
        constexpr std::uint16_t kSymProcIdEnd     = 0x114F;
        constexpr std::uint16_t kSymInlineSite2   = 0x115D;

        template <typename T>
        T Read( std::span<const std::byte> bytes, std::size_t at )
        {
            T value{};
            if ( at + sizeof( T ) <= bytes.size() )
            {
                std::memcpy( &value, bytes.data() + at, sizeof( T ) );
            }
            return value;
        }

        // The compiler's spelling of a path ("F:\\DesertEngine\\Desert\\VFX\\...") against the index's: case and
        // separators fold (NTFS is case-insensitive).
        std::string Fold( std::string_view path )
        {
            std::string out( path );
            for ( char& c : out )
            {
                c = c == '\\' ? '/' : char( std::tolower( static_cast<unsigned char>( c ) ) );
            }
            return out;
        }

        bool IsProc( std::uint16_t kind )
        {
            return kind == kSymGProc32 || kind == kSymLProc32 || kind == kSymGProc32Id || kind == kSymLProc32Id;
        }

        bool OpensScope( std::uint16_t kind )
        {
            return IsProc( kind ) || kind == kSymThunk32 || kind == kSymBlock32 || kind == kSymWith32 ||
                   kind == kSymSepCode || kind == kSymInlineSite || kind == kSymInlineSite2;
        }

        bool ClosesScope( std::uint16_t kind )
        {
            return kind == kSymEnd || kind == kSymProcIdEnd || kind == kSymInlineSiteEnd;
        }

        struct Procedure
        {
            std::uint32_t              Symbol = 0; // the object's symbol of the function
            std::vector<std::uint32_t> Inlinees;   // item ids, every depth
        };

        struct ObjectDebug
        {
            std::span<const std::byte>                                    Strings;
            std::span<const std::byte>                                    Files;
            std::unordered_map<std::uint32_t, std::uint32_t>              OwnFile;      // symbol -> fileId
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> InlineeFiles; // item id -> fileIds
            std::vector<Procedure>                                        Procedures;
        };

        void ReadSymbols( std::span<const std::byte> data, std::size_t dataOffset,
                          const std::unordered_map<std::uint32_t, std::uint32_t>& secrel, ObjectDebug& debug )
        {
            std::size_t at    = 0;
            int         depth = 0;
            Procedure*  open  = nullptr;
            while ( at + 4 <= data.size() )
            {
                const std::uint16_t length = Read<std::uint16_t>( data, at );
                const std::uint16_t kind   = Read<std::uint16_t>( data, at + 2 );
                if ( length < 2 )
                {
                    break;
                }
                if ( IsProc( kind ) && depth == 0 )
                {
                    const auto symbol = secrel.find( std::uint32_t( dataOffset + at + 32 ) );
                    if ( symbol != secrel.end() )
                    {
                        debug.Procedures.push_back( Procedure{ symbol->second, {} } );
                        open = &debug.Procedures.back();
                    }
                    else
                    {
                        open = nullptr;
                    }
                }
                else if ( ( kind == kSymInlineSite || kind == kSymInlineSite2 ) && open != nullptr )
                {
                    open->Inlinees.push_back( Read<std::uint32_t>( data, at + 12 ) );
                }
                if ( OpensScope( kind ) )
                {
                    ++depth;
                }
                else if ( ClosesScope( kind ) && depth > 0 )
                {
                    if ( --depth == 0 )
                    {
                        open = nullptr;
                    }
                }
                at += std::size_t( length ) + 2;
            }
        }

        void ReadInlinees( std::span<const std::byte> data, ObjectDebug& debug )
        {
            const std::uint32_t signature = Read<std::uint32_t>( data, 0 );
            std::size_t         at        = 4;
            while ( at + 12 <= data.size() )
            {
                const std::uint32_t         inlinee = Read<std::uint32_t>( data, at );
                std::vector<std::uint32_t>& files   = debug.InlineeFiles[inlinee];
                files.push_back( Read<std::uint32_t>( data, at + 4 ) );
                at += 12;
                if ( signature == 1 )
                {
                    const std::uint32_t extra = Read<std::uint32_t>( data, at );
                    at += 4;
                    for ( std::uint32_t i = 0; i < extra && at + 4 <= data.size(); ++i, at += 4 )
                    {
                        files.push_back( Read<std::uint32_t>( data, at ) );
                    }
                }
            }
        }

        std::string FileName( const ObjectDebug& debug, std::uint32_t fileId )
        {
            const std::uint32_t name = Read<std::uint32_t>( debug.Files, fileId );
            if ( fileId + 4 > debug.Files.size() || name >= debug.Strings.size() )
            {
                return {};
            }
            const char*       begin = reinterpret_cast<const char*>( debug.Strings.data() + name );
            const std::size_t limit = debug.Strings.size() - name;
            return std::string( begin, strnlen( begin, limit ) );
        }
    } // namespace

    Common::ResultStr<Carriers> FindCarriers( const Coff::Object&                    object,
                                              std::span<const std::filesystem::path> sources )
    {
        ObjectDebug debug;
        bool        sawDebug = false;
        for ( const Coff::Section& section : object.Sections() )
        {
            if ( section.Name != ".debug$S" || section.Bytes.size() < 4 )
            {
                continue;
            }
            if ( Read<std::uint32_t>( section.Bytes, 0 ) != kSignatureC13 )
            {
                return Common::MakeFormattedError<Carriers>( "{}: .debug$S is not CodeView C13 (compile with /Z7)",
                                                             object.Path().generic_string() );
            }
            sawDebug = true;
            std::unordered_map<std::uint32_t, std::uint32_t> secrel; // section offset -> symbol
            for ( const Coff::Relocation& relocation : section.Relocations )
            {
                if ( relocation.Kind == Coff::RelocationKind::SecRel )
                {
                    secrel.emplace( relocation.Offset, relocation.Symbol );
                }
            }

            std::size_t at = 4;
            while ( at + 8 <= section.Bytes.size() )
            {
                const std::uint32_t kind   = Read<std::uint32_t>( section.Bytes, at );
                const std::uint32_t length = Read<std::uint32_t>( section.Bytes, at + 4 );
                const std::size_t   begin  = at + 8;
                if ( begin + length > section.Bytes.size() )
                {
                    return Common::MakeFormattedError<Carriers>( "{}: a .debug$S subsection runs past its section",
                                                                 object.Path().generic_string() );
                }
                const std::span<const std::byte> data = section.Bytes.subspan( begin, length );
                switch ( kind & ~kIgnoreSubsection )
                {
                    case kSubsectionSymbols:
                        if ( ( kind & kIgnoreSubsection ) == 0 )
                        {
                            ReadSymbols( data, begin, secrel, debug );
                        }
                        break;
                    case kSubsectionLines:
                    {
                        // The first file block is the function's own file; offCon's SECREL names the function.
                        const auto symbol = secrel.find( std::uint32_t( begin ) );
                        if ( symbol != secrel.end() && length >= 16 )
                        {
                            debug.OwnFile.try_emplace( symbol->second, Read<std::uint32_t>( data, 12 ) );
                        }
                        break;
                    }
                    case kSubsectionStrings:
                        debug.Strings = data;
                        break;
                    case kSubsectionFiles:
                        debug.Files = data;
                        break;
                    case kSubsectionInlinee:
                        ReadInlinees( data, debug );
                        break;
                    default:
                        break;
                }
                at = ( begin + length + 3 ) & ~std::size_t( 3 );
            }
        }
        if ( !sawDebug )
        {
            return Common::MakeFormattedError<Carriers>( "{} has no .debug$S (compile with /Z7)",
                                                         object.Path().generic_string() );
        }

        std::vector<std::string> wanted;
        wanted.reserve( sources.size() );
        for ( const std::filesystem::path& source : sources )
        {
            wanted.push_back( Fold( source.generic_string() ) );
        }
        std::unordered_map<std::uint32_t, bool> fileWanted; // fileId -> in @p sources
        const auto                              isWanted = [&]( std::uint32_t fileId )
        {
            const auto known = fileWanted.find( fileId );
            if ( known != fileWanted.end() )
            {
                return known->second;
            }
            const std::string name = Fold( FileName( debug, fileId ) );
            const bool        hit  = std::find( wanted.begin(), wanted.end(), name ) != wanted.end();
            fileWanted.emplace( fileId, hit );
            return hit;
        };

        Carriers                            carriers;
        std::unordered_set<std::string>     files;
        const std::span<const Coff::Symbol> symbols = object.Symbols();
        for ( const Procedure& procedure : debug.Procedures )
        {
            if ( procedure.Symbol >= symbols.size() )
            {
                continue;
            }
            const auto own     = debug.OwnFile.find( procedure.Symbol );
            const bool ownHit  = own != debug.OwnFile.end() && isWanted( own->second );
            bool       carries = ownHit;
            for ( std::size_t i = 0; !carries && i < procedure.Inlinees.size(); ++i )
            {
                const auto inlinee = debug.InlineeFiles.find( procedure.Inlinees[i] );
                if ( inlinee == debug.InlineeFiles.end() )
                {
                    continue;
                }
                carries = std::any_of( inlinee->second.begin(), inlinee->second.end(), isWanted );
            }
            if ( !carries )
            {
                continue;
            }
            carriers.Functions.insert( symbols[procedure.Symbol].Name );
            if ( !ownHit && own != debug.OwnFile.end() )
            {
                const std::string name = FileName( debug, own->second );
                if ( !name.empty() && files.insert( Fold( name ) ).second )
                {
                    carriers.Files.emplace_back( name );
                }
            }
        }
        return Common::MakeSuccess( std::move( carriers ) );
    }
} // namespace Desert::Editor::LiveCoding::CodeView

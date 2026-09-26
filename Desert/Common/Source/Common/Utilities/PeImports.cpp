#include "PeImports.hpp"

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

namespace Common::Utils
{
    namespace
    {
        // Offsets from the PE/COFF specification (Microsoft, "PE Format").
        constexpr std::size_t   kDosLfanewOffset      = 0x3C;
        constexpr std::size_t   kCoffHeaderSize       = 20;
        constexpr std::size_t   kSectionHeaderSize    = 40;
        constexpr std::uint16_t kMagicPe32            = 0x10B;
        constexpr std::uint16_t kMagicPe32Plus        = 0x20B;
        constexpr std::uint32_t kImportDirectory      = 1;
        constexpr std::uint32_t kDelayImportDirectory = 13;
        constexpr std::size_t   kImportDescriptorSize = 20;
        constexpr std::size_t   kDelayDescriptorSize  = 32;
        // A DLL name longer than this is not a name; it is a reader walking off into data.
        constexpr std::size_t kMaxDllNameLength = 260;

        struct Section
        {
            std::uint32_t VirtualAddress;
            std::uint32_t VirtualSize;
            std::uint32_t RawOffset;
            std::uint32_t RawSize;
        };

        class Image
        {
        public:
            explicit Image( std::span<const std::uint8_t> bytes ) : m_Bytes( bytes )
            {
            }

            [[nodiscard]] std::optional<std::uint16_t> U16( std::size_t at ) const
            {
                if ( at + 2 > m_Bytes.size() )
                    return std::nullopt;
                return static_cast<std::uint16_t>( m_Bytes[at] | ( m_Bytes[at + 1] << 8 ) );
            }

            [[nodiscard]] std::optional<std::uint32_t> U32( std::size_t at ) const
            {
                if ( at + 4 > m_Bytes.size() )
                    return std::nullopt;
                return static_cast<std::uint32_t>( m_Bytes[at] ) |
                       ( static_cast<std::uint32_t>( m_Bytes[at + 1] ) << 8 ) |
                       ( static_cast<std::uint32_t>( m_Bytes[at + 2] ) << 16 ) |
                       ( static_cast<std::uint32_t>( m_Bytes[at + 3] ) << 24 );
            }

            [[nodiscard]] std::optional<std::uint64_t> U64( std::size_t at ) const
            {
                const auto lo = U32( at );
                const auto hi = U32( at + 4 );
                if ( !lo || !hi )
                    return std::nullopt;
                return static_cast<std::uint64_t>( *lo ) | ( static_cast<std::uint64_t>( *hi ) << 32 );
            }

            [[nodiscard]] bool AllZero( std::size_t at, std::size_t count ) const
            {
                return std::all_of( m_Bytes.begin() + static_cast<std::ptrdiff_t>( at ),
                                    m_Bytes.begin() + static_cast<std::ptrdiff_t>( at + count ),
                                    []( std::uint8_t b ) { return b == 0; } );
            }

            [[nodiscard]] std::size_t Size() const
            {
                return m_Bytes.size();
            }

            [[nodiscard]] std::optional<std::string> CString( std::size_t at ) const
            {
                std::string text;
                for ( std::size_t i = at; i < m_Bytes.size() && text.size() <= kMaxDllNameLength; ++i )
                {
                    if ( m_Bytes[i] == 0 )
                        return text;
                    text.push_back( static_cast<char>( m_Bytes[i] ) );
                }
                return std::nullopt;
            }

        private:
            std::span<const std::uint8_t> m_Bytes;
        };

        std::optional<std::size_t> RvaToOffset( const std::vector<Section>& sections, std::uint32_t rva )
        {
            for ( const Section& s : sections )
            {
                const std::uint32_t extent = std::max( s.VirtualSize, s.RawSize );
                if ( rva >= s.VirtualAddress && rva - s.VirtualAddress < extent )
                {
                    const std::uint32_t inSection = rva - s.VirtualAddress;
                    if ( inSection >= s.RawSize )
                        return std::nullopt; // uninitialised tail: no bytes on disk
                    return static_cast<std::size_t>( s.RawOffset ) + inSection;
                }
            }
            return std::nullopt;
        }

        std::string Lower( std::string text )
        {
            std::transform( text.begin(), text.end(), text.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return text;
        }
    } // namespace

    Common::ResultStr<std::vector<std::string>> ReadPeImports( std::span<const std::uint8_t> bytes )
    {
        using Names = std::vector<std::string>;
        const Image image( bytes );

        if ( image.Size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z' )
            return Common::MakeFormattedError<Names>( "not a PE image: no 'MZ' at offset 0 ({} bytes)",
                                                      image.Size() );
        const auto lfanew = image.U32( kDosLfanewOffset );
        if ( !lfanew )
            return Common::MakeFormattedError<Names>( "not a PE image: truncated DOS header ({} bytes)",
                                                      image.Size() );
        const std::size_t pe = *lfanew;
        if ( pe + 4 + kCoffHeaderSize > image.Size() || std::memcmp( bytes.data() + pe, "PE\0\0", 4 ) != 0 )
            return Common::MakeFormattedError<Names>( "not a PE image: no 'PE\\0\\0' signature at offset {}", pe );

        const std::size_t coff         = pe + 4;
        const auto        sectionCount = image.U16( coff + 2 );
        const auto        optionalSize = image.U16( coff + 16 );
        const std::size_t optional     = coff + kCoffHeaderSize;
        const auto        magic        = image.U16( optional );
        if ( !sectionCount || !optionalSize || !magic )
            return Common::MakeFormattedError<Names>( "truncated COFF/optional header at offset {}", coff );

        const bool plus = *magic == kMagicPe32Plus;
        if ( !plus && *magic != kMagicPe32 )
            return Common::MakeFormattedError<Names>( "unknown optional-header magic 0x{:X} at offset {}", *magic,
                                                      optional );

        // PE32+ stores ImageBase as 64 bits at +24; PE32 as 32 bits at +28 (BaseOfData sits at +24).
        std::optional<std::uint64_t> imageBase;
        if ( plus )
            imageBase = image.U64( optional + 24 );
        else if ( const auto base32 = image.U32( optional + 28 ) )
            imageBase = *base32;
        const std::size_t dirCountAt = optional + ( plus ? 108 : 92 );
        const std::size_t dirsAt     = optional + ( plus ? 112 : 96 );
        const auto        dirCount   = image.U32( dirCountAt );
        if ( !imageBase || !dirCount )
            return Common::MakeFormattedError<Names>( "truncated optional header at offset {}", optional );

        std::vector<Section> sections;
        const std::size_t    sectionTable = optional + *optionalSize;
        for ( std::size_t i = 0; i < *sectionCount; ++i )
        {
            const std::size_t at   = sectionTable + i * kSectionHeaderSize;
            const auto        vsz  = image.U32( at + 8 );
            const auto        va   = image.U32( at + 12 );
            const auto        rsz  = image.U32( at + 16 );
            const auto        roff = image.U32( at + 20 );
            if ( !vsz || !va || !rsz || !roff )
                return Common::MakeFormattedError<Names>( "section header {} of {} is truncated (offset {})", i,
                                                          *sectionCount, at );
            sections.push_back( { *va, *vsz, *roff, *rsz } );
        }

        Names                 names;
        std::set<std::string> seen;
        auto addName = [&]( std::uint32_t nameRva, const char* table ) -> std::optional<std::string>
        {
            const auto at = RvaToOffset( sections, nameRva );
            if ( !at )
                return std::string( table ) + " names RVA 0x" + fmt::format( "{:X}", nameRva ) +
                       ", outside every section";
            const auto name = image.CString( *at );
            if ( !name || name->empty() )
                return std::string( table ) + " name at offset " + std::to_string( *at ) +
                       " is unterminated or empty";
            if ( seen.insert( Lower( *name ) ).second )
                names.push_back( *name );
            return std::nullopt;
        };

        // Walks one descriptor array. A directory that is absent (RVA 0) is an image without that table.
        auto walk = [&]( std::uint32_t directory, std::size_t descriptorSize,
                         const char* table ) -> std::optional<std::string>
        {
            if ( directory >= *dirCount )
                return std::nullopt;
            const auto rva = image.U32( dirsAt + static_cast<std::size_t>( directory ) * 8 );
            if ( !rva )
                return std::string( "data directory " ) + std::to_string( directory ) + " is truncated";
            if ( *rva == 0 )
                return std::nullopt;
            const auto start = RvaToOffset( sections, *rva );
            if ( !start )
                return std::string( table ) + " RVA 0x" + fmt::format( "{:X}", *rva ) +
                       " is outside every section";
            for ( std::size_t at = *start;; at += descriptorSize )
            {
                if ( at + descriptorSize > image.Size() )
                    return std::string( table ) + " runs past the end of the file at offset " +
                           std::to_string( at );
                if ( image.AllZero( at, descriptorSize ) )
                    return std::nullopt;
                std::uint32_t nameRva = 0;
                if ( descriptorSize == kImportDescriptorSize )
                    nameRva = *image.U32( at + 12 );
                else
                {
                    // Attributes bit 0 set: the fields are RVAs (every modern linker). Clear: VC6-era
                    // virtual addresses, relative to the image base.
                    const std::uint32_t attributes = *image.U32( at );
                    const std::uint32_t field      = *image.U32( at + 4 );
                    nameRva = ( attributes & 1u ) != 0 ? field : static_cast<std::uint32_t>( field - *imageBase );
                }
                if ( auto failed = addName( nameRva, table ) )
                    return failed;
            }
        };

        if ( auto failed = walk( kImportDirectory, kImportDescriptorSize, "import table" ) )
            return Common::MakeError<Names>( *failed );
        if ( auto failed = walk( kDelayImportDirectory, kDelayDescriptorSize, "delay-load import table" ) )
            return Common::MakeError<Names>( *failed );
        return Common::MakeSuccess( std::move( names ) );
    }

    Common::ResultStr<std::vector<std::string>> ReadPeImportsOfFile( const std::filesystem::path& file )
    {
        auto content = FileSystem::ReadFileContent( file );
        if ( !content )
            return Common::MakeFormattedError<std::vector<std::string>>( "{} could not be read: {}", file.string(),
                                                                         content.GetError() );
        // The reader takes unsigned bytes; the file arrives as chars. Copying (an exe is megabytes, read
        // once per package) keeps this free of a pointer reinterpretation.
        const std::string               text = content.ExtractValue();
        const std::vector<std::uint8_t> bytes( text.begin(), text.end() );
        auto                            names = ReadPeImports( bytes );
        if ( !names )
            return Common::MakeFormattedError<std::vector<std::string>>( "{}: {}", file.string(),
                                                                         names.GetError() );
        return names;
    }

    Common::ResultStr<std::vector<std::filesystem::path>>
    AppLocalRuntimeClosure( const std::filesystem::path& exe, const std::filesystem::path& crtDir )
    {
        using Paths  = std::vector<std::filesystem::path>;
        namespace fs = std::filesystem;

        // lower-case file name -> the file, for every DLL the redistributable directory holds.
        std::map<std::string, fs::path> redistributable;
        std::error_code                 ec;
        for ( fs::directory_iterator it( crtDir, ec ), end; !ec && it != end; it.increment( ec ) )
        {
            const std::string name = Lower( it->path().filename().string() );
            if ( it->is_regular_file( ec ) && name.size() > 4 && name.ends_with( ".dll" ) )
                redistributable.emplace( name, it->path() );
        }
        if ( ec )
            return Common::MakeFormattedError<Paths>( "the VC++ runtime directory {} could not be listed: {}",
                                                      crtDir.string(), ec.message() );
        if ( redistributable.empty() )
            return Common::MakeFormattedError<Paths>( "the VC++ runtime directory {} holds no .dll files",
                                                      crtDir.string() );

        auto isDebugCrt = [&]( const std::string& lower )
        {
            if ( lower == "ucrtbased.dll" )
                return true;
            constexpr std::string_view suffix = "d.dll";
            if ( lower.size() <= suffix.size() || !lower.ends_with( suffix ) )
                return false;
            return redistributable.contains( lower.substr( 0, lower.size() - suffix.size() ) + ".dll" );
        };

        std::set<std::string>    shipped;
        std::vector<fs::path>    pending{ exe };
        std::vector<std::string> debugImports;
        bool                     exeRead = false;
        while ( !pending.empty() )
        {
            const fs::path binary = pending.back();
            pending.pop_back();
            auto imports = ReadPeImportsOfFile( binary );
            if ( !imports )
                return Common::MakeError<Paths>( imports.GetError() );
            if ( !exeRead && imports.GetValue().empty() )
                return Common::MakeFormattedError<Paths>(
                     "{} imports no DLL at all — every Windows image imports KERNEL32, so its import table was "
                     "not understood",
                     binary.string() );
            exeRead = true;
            for ( const std::string& dll : imports.GetValue() )
            {
                const std::string lower = Lower( dll );
                if ( isDebugCrt( lower ) )
                    debugImports.push_back( dll + " (imported by " + binary.filename().string() + ")" );
                else if ( const auto found = redistributable.find( lower );
                          found != redistributable.end() && shipped.insert( lower ).second )
                    pending.push_back( found->second );
            }
        }

        if ( !debugImports.empty() )
        {
            std::string list;
            for ( const std::string& entry : debugImports )
                list += "\n  " + entry;
            return Common::MakeFormattedError<Paths>(
                 "{} is linked against the DEBUG C++ runtime, which Microsoft does not license for redistribution "
                 "and which exists only where Visual Studio is installed — package a Release or Shipping build:{}",
                 exe.filename().string(), list );
        }

        Paths result;
        for ( const std::string& name : shipped )
            result.push_back( redistributable.at( name ) );
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Common::Utils

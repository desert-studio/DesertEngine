#include "CoffObject.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

// PE/COFF object files (Microsoft PE/COFF Specification §3 Section Table, §4 COFF Symbol Table, §5.2 COFF
// Relocations, §5.5.6 COMDAT Sections) and the /bigobj header (ANON_OBJECT_HEADER_BIGOBJ, winnt.h). Read by hand:
// the editor links against nothing that parses objects, and the layouts below are fixed by the specification.
namespace Desert::Editor::LiveCoding::Coff
{
    namespace
    {
        constexpr std::uint32_t kScnLnkNRelocOvfl  = 0x01000000; // IMAGE_SCN_LNK_NRELOC_OVFL
        constexpr std::uint32_t kScnUninitialised  = 0x00000080; // IMAGE_SCN_CNT_UNINITIALIZED_DATA
        constexpr std::uint32_t kScnLnkComdat      = 0x00001000; // IMAGE_SCN_LNK_COMDAT
        constexpr std::uint16_t kMachineAmd64      = 0x8664;
        constexpr std::size_t   kFileHeaderSize    = 20;
        constexpr std::size_t   kBigObjHeaderSize  = 56;
        constexpr std::size_t   kSectionHeaderSize = 40;
        constexpr std::size_t   kRelocationSize    = 10;
        constexpr std::uint8_t  kSymClassStatic    = 3; // IMAGE_SYM_CLASS_STATIC
        constexpr std::uint8_t  kSymClassWeakExternal = 105; // IMAGE_SYM_CLASS_WEAK_EXTERNAL
        constexpr std::uint16_t kSymDtypeFunction  = 2; // IMAGE_SYM_DTYPE_FUNCTION, in bits 4..5 of Type

        // {D1BAA1C7-BAEE-4ba9-AF20-FAF66AA4DCB8}, the ClassID of ANON_OBJECT_HEADER_BIGOBJ.
        constexpr std::array<std::uint8_t, 16> kBigObjClassId = { 0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                                                  0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8 };

        template <typename T>
        T Read( const std::vector<std::byte>& bytes, std::size_t at )
        {
            T value{};
            std::memcpy( &value, bytes.data() + at, sizeof( T ) );
            return value;
        }

        bool Fits( const std::vector<std::byte>& bytes, std::size_t at, std::size_t size )
        {
            return at <= bytes.size() && size <= bytes.size() - at;
        }

        // An 8-byte short name, or "/123" (decimal offset into the string table) for a section name.
        std::string ShortName( const std::byte* name )
        {
            std::size_t length = 0;
            while ( length < 8 && name[length] != std::byte{ 0 } )
            {
                ++length;
            }
            return std::string( reinterpret_cast<const char*>( name ), length );
        }

        std::string StringAt( const std::vector<std::byte>& bytes, std::size_t table, std::size_t tableSize,
                              std::size_t offset )
        {
            if ( offset >= tableSize )
            {
                return {};
            }
            const char*       begin = reinterpret_cast<const char*>( bytes.data() + table + offset );
            const std::size_t limit = tableSize - offset;
            return std::string( begin, strnlen( begin, limit ) );
        }
    } // namespace

    Common::ResultStr<Object> Object::Load( const std::filesystem::path& path )
    {
        Object object;
        object.m_Path = path;
        {
            std::ifstream file( path, std::ios::binary | std::ios::ate );
            if ( !file )
            {
                return Common::MakeFormattedError<Object>( "cannot open the object {}", path.generic_string() );
            }
            const std::streamoff size = file.tellg();
            object.m_Bytes.resize( static_cast<std::size_t>( size ) );
            file.seekg( 0 );
            if ( !file.read( reinterpret_cast<char*>( object.m_Bytes.data() ), size ) )
            {
                return Common::MakeFormattedError<Object>( "cannot read the object {}", path.generic_string() );
            }
        }
        const std::vector<std::byte>& bytes = object.m_Bytes;
        const std::string             name  = path.generic_string();
        if ( bytes.size() < kFileHeaderSize )
        {
            return Common::MakeFormattedError<Object>( "{} is too short for a COFF header", name );
        }

        // The two header forms: IMAGE_FILE_HEADER, or ANON_OBJECT_HEADER_BIGOBJ (Sig1 0, Sig2 0xFFFF, ClassID).
        const bool bigObj = Read<std::uint16_t>( bytes, 0 ) == 0 && Read<std::uint16_t>( bytes, 2 ) == 0xFFFF &&
                            Fits( bytes, 0, kBigObjHeaderSize ) &&
                            std::memcmp( bytes.data() + 12, kBigObjClassId.data(), kBigObjClassId.size() ) == 0;
        std::uint16_t machine      = 0;
        std::uint32_t sectionCount = 0;
        std::uint32_t symbolTable  = 0;
        std::uint32_t symbolCount  = 0;
        std::size_t   firstSection = 0;
        if ( bigObj )
        {
            machine      = Read<std::uint16_t>( bytes, 6 );
            sectionCount = Read<std::uint32_t>( bytes, 44 );
            symbolTable  = Read<std::uint32_t>( bytes, 48 );
            symbolCount  = Read<std::uint32_t>( bytes, 52 );
            firstSection = kBigObjHeaderSize;
        }
        else
        {
            machine      = Read<std::uint16_t>( bytes, 0 );
            sectionCount = Read<std::uint16_t>( bytes, 2 );
            symbolTable  = Read<std::uint32_t>( bytes, 8 );
            symbolCount  = Read<std::uint32_t>( bytes, 12 );
            firstSection = kFileHeaderSize + Read<std::uint16_t>( bytes, 16 );
        }
        if ( machine != kMachineAmd64 )
        {
            return Common::MakeFormattedError<Object>( "{} is not an x64 object (machine {:#x})", name, machine );
        }

        const std::size_t symbolSize = bigObj ? 20 : 18;
        if ( !Fits( bytes, symbolTable, std::size_t( symbolCount ) * symbolSize + 4 ) ||
             !Fits( bytes, firstSection, std::size_t( sectionCount ) * kSectionHeaderSize ) )
        {
            return Common::MakeFormattedError<Object>( "{}: the section or symbol table runs past the file",
                                                       name );
        }
        const std::size_t stringTable     = symbolTable + std::size_t( symbolCount ) * symbolSize;
        const std::size_t stringTableSize = Read<std::uint32_t>( bytes, stringTable );
        if ( !Fits( bytes, stringTable, stringTableSize ) )
        {
            return Common::MakeFormattedError<Object>( "{}: the string table runs past the file", name );
        }

        object.m_Sections.reserve( sectionCount );
        for ( std::uint32_t i = 0; i < sectionCount; ++i )
        {
            const std::size_t header = firstSection + std::size_t( i ) * kSectionHeaderSize;
            Section           section;
            section.Name = ShortName( bytes.data() + header );
            if ( !section.Name.empty() && section.Name[0] == '/' )
            {
                section.Name = StringAt( bytes, stringTable, stringTableSize,
                                         std::strtoul( section.Name.c_str() + 1, nullptr, 10 ) );
            }
            section.Size                        = Read<std::uint32_t>( bytes, header + 16 );
            const std::uint32_t rawData         = Read<std::uint32_t>( bytes, header + 20 );
            const std::uint32_t relocations     = Read<std::uint32_t>( bytes, header + 24 );
            std::uint32_t       relocationCount = Read<std::uint16_t>( bytes, header + 32 );
            section.Characteristics             = Read<std::uint32_t>( bytes, header + 36 );

            if ( ( section.Characteristics & kScnUninitialised ) == 0 && section.Size != 0 )
            {
                if ( !Fits( bytes, rawData, section.Size ) )
                {
                    return Common::MakeFormattedError<Object>( "{}: section {} runs past the file", name,
                                                               section.Name );
                }
                section.Bytes = std::span<const std::byte>( bytes.data() + rawData, section.Size );
            }

            std::size_t firstRelocation = relocations;
            if ( ( section.Characteristics & kScnLnkNRelocOvfl ) != 0 && relocationCount == 0xFFFF )
            {
                // The real count is the first relocation's VirtualAddress, and it counts itself.
                if ( !Fits( bytes, relocations, kRelocationSize ) )
                {
                    return Common::MakeFormattedError<Object>( "{}: section {} relocations run past the file",
                                                               name, section.Name );
                }
                relocationCount = Read<std::uint32_t>( bytes, relocations ) - 1;
                firstRelocation += kRelocationSize;
            }
            if ( !Fits( bytes, firstRelocation, std::size_t( relocationCount ) * kRelocationSize ) )
            {
                return Common::MakeFormattedError<Object>( "{}: section {} relocations run past the file", name,
                                                           section.Name );
            }
            section.Relocations.reserve( relocationCount );
            for ( std::uint32_t r = 0; r < relocationCount; ++r )
            {
                const std::size_t at = firstRelocation + std::size_t( r ) * kRelocationSize;
                section.Relocations.push_back(
                     Relocation{ Read<std::uint32_t>( bytes, at ), Read<std::uint32_t>( bytes, at + 4 ),
                                 RelocationKind( Read<std::uint16_t>( bytes, at + 8 ) ) } );
            }
            object.m_Sections.push_back( std::move( section ) );
        }

        object.m_Symbols.reserve( symbolCount );
        for ( std::uint32_t i = 0; i < symbolCount; ++i )
        {
            const std::size_t at = symbolTable + std::size_t( i ) * symbolSize;
            Symbol            symbol;
            if ( Read<std::uint32_t>( bytes, at ) == 0 )
            {
                symbol.Name =
                     StringAt( bytes, stringTable, stringTableSize, Read<std::uint32_t>( bytes, at + 4 ) );
            }
            else
            {
                symbol.Name = ShortName( bytes.data() + at );
            }
            symbol.Value     = Read<std::uint32_t>( bytes, at + 8 );
            std::size_t next = at + 12;
            if ( bigObj )
            {
                symbol.Section = Read<std::int32_t>( bytes, next );
                next += 4;
            }
            else
            {
                // Unsigned: a regular object numbers up to 65279 sections; only the two reserved values are
                // negative (IMAGE_SYM_ABSOLUTE 0xFFFF, IMAGE_SYM_DEBUG 0xFFFE).
                const std::uint16_t number = Read<std::uint16_t>( bytes, next );
                symbol.Section = number == 0xFFFF ? -1 : number == 0xFFFE ? -2 : std::int32_t( number );
                next += 2;
            }
            const std::uint16_t type    = Read<std::uint16_t>( bytes, next );
            symbol.StorageClass         = Read<std::uint8_t>( bytes, next + 2 );
            const std::uint8_t auxCount = Read<std::uint8_t>( bytes, next + 3 );
            symbol.Function             = ( ( type >> 4 ) & 0x3 ) == kSymDtypeFunction;

            // The section definition of a COMDAT: the static symbol named like the section, value 0, whose first
            // aux record carries Number (+ HighNumber for /bigobj) and Selection.
            if ( auxCount > 0 && symbol.StorageClass == kSymClassStatic && symbol.Value == 0 &&
                 symbol.Section > 0 && std::uint32_t( symbol.Section ) <= object.m_Sections.size() )
            {
                Section& section = object.m_Sections[std::size_t( symbol.Section ) - 1];
                if ( ( section.Characteristics & kScnLnkComdat ) != 0 && section.Comdat == ComdatSelect::None )
                {
                    const std::size_t aux       = at + symbolSize;
                    std::uint32_t     associate = Read<std::uint16_t>( bytes, aux + 12 );
                    if ( bigObj )
                    {
                        associate |= std::uint32_t( Read<std::uint16_t>( bytes, aux + 16 ) ) << 16;
                    }
                    section.Comdat         = ComdatSelect( Read<std::uint8_t>( bytes, aux + 14 ) );
                    section.AssociatedWith = associate;
                }
            }

            // A weak external's first aux record names the symbol it falls back to (TagIndex, §5.5.3).
            if ( auxCount > 0 && symbol.StorageClass == kSymClassWeakExternal && symbol.Section == 0 )
            {
                symbol.WeakDefault = Read<std::uint32_t>( bytes, at + symbolSize );
            }

            object.m_Symbols.push_back( std::move( symbol ) );
            for ( std::uint8_t a = 0; a < auxCount && i + 1 < symbolCount; ++a )
            {
                object.m_Symbols.push_back( Symbol{} );
                ++i;
            }
        }
        object.ComputeExtents();
        return Common::MakeSuccess( std::move( object ) );
    }

    // Once per object: every defined symbol's (section, value) sorted, so each extent is the gap to the next
    // larger value of its section, or to the section end.
    void Object::ComputeExtents()
    {
        std::vector<std::pair<std::int32_t, std::uint32_t>> starts;
        starts.reserve( m_Symbols.size() );
        for ( const Symbol& symbol : m_Symbols )
        {
            if ( symbol.Section > 0 && std::size_t( symbol.Section ) <= m_Sections.size() )
            {
                starts.emplace_back( symbol.Section, symbol.Value );
            }
        }
        std::sort( starts.begin(), starts.end() );
        m_Extents.assign( m_Symbols.size(), 0 );
        for ( std::size_t i = 0; i < m_Symbols.size(); ++i )
        {
            const Symbol& self = m_Symbols[i];
            if ( self.Section <= 0 || std::size_t( self.Section ) > m_Sections.size() )
            {
                continue;
            }
            std::uint32_t end = m_Sections[std::size_t( self.Section ) - 1].Size;
            const auto    next =
                 std::upper_bound( starts.begin(), starts.end(), std::make_pair( self.Section, self.Value ) );
            if ( next != starts.end() && next->first == self.Section && next->second < end )
            {
                end = next->second;
            }
            m_Extents[i] = end > self.Value ? end - self.Value : 0;
        }
    }

    std::uint32_t Object::Extent( std::uint32_t symbol ) const
    {
        return symbol < m_Extents.size() ? m_Extents[symbol] : 0;
    }
} // namespace Desert::Editor::LiveCoding::Coff

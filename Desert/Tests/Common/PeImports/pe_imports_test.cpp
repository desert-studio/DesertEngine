#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PeImports.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

// The images here are built byte by byte from the PE/COFF specification, so the reader is checked
// against the format and not against itself: one .idata-style section at RVA 0x1000 / file offset 0x200
// holding the import descriptors, the delay-load descriptors and the name strings.

namespace fs = std::filesystem;
using Common::Utils::AppLocalRuntimeClosure;
using Common::Utils::ReadPeImports;

namespace
{
    constexpr std::uint32_t kSectionRva    = 0x1000;
    constexpr std::uint32_t kSectionOffset = 0x200;
    constexpr std::uint32_t kDelayRva      = 0x1100;
    constexpr std::uint32_t kNamesRva      = 0x1200;
    constexpr std::size_t   kFileSize      = 0x600;

    struct PeSpec
    {
        // Defaulted so a case names only the tables it builds.
        std::vector<std::string> Imports{};
        std::vector<std::string> DelayImports{};
        bool                     Pe32 = false;
        // Replaces the first import descriptor's name RVA, to build a corrupt image.
        std::uint32_t BadNameRva = 0;
    };

    void Put16( std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v )
    {
        b[at]     = static_cast<std::uint8_t>( v );
        b[at + 1] = static_cast<std::uint8_t>( v >> 8 );
    }

    void Put32( std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v )
    {
        for ( int i = 0; i < 4; ++i )
            b[at + i] = static_cast<std::uint8_t>( v >> ( 8 * i ) );
    }

    std::size_t At( std::uint32_t rva )
    {
        return kSectionOffset + ( rva - kSectionRva );
    }

    std::vector<std::uint8_t> MakePe( const PeSpec& spec )
    {
        std::vector<std::uint8_t> b( kFileSize, 0 );
        b[0] = 'M';
        b[1] = 'Z';
        Put32( b, 0x3C, 0x40 );
        b[0x40]                          = 'P';
        b[0x41]                          = 'E';
        const std::size_t   coff         = 0x44;
        const std::size_t   optional     = coff + 20;
        const std::uint16_t optionalSize = spec.Pe32 ? 0xE0 : 0xF0;
        Put16( b, coff, spec.Pe32 ? 0x14C : 0x8664 );
        Put16( b, coff + 2, 1 );
        Put16( b, coff + 16, optionalSize );
        Put16( b, optional, spec.Pe32 ? 0x10B : 0x20B );
        Put32( b, optional + ( spec.Pe32 ? 92 : 108 ), 16 );
        const std::size_t dirs = optional + ( spec.Pe32 ? 96 : 112 );
        if ( !spec.Imports.empty() )
            Put32( b, dirs + std::size_t{ 1 } * 8, kSectionRva );
        if ( !spec.DelayImports.empty() )
            Put32( b, dirs + std::size_t{ 13 } * 8, kDelayRva );

        const std::size_t section = optional + optionalSize;
        Put32( b, section + 8, 0x400 );
        Put32( b, section + 12, kSectionRva );
        Put32( b, section + 16, 0x400 );
        Put32( b, section + 20, kSectionOffset );

        std::uint32_t nameRva   = kNamesRva;
        auto          writeName = [&]( const std::string& name )
        {
            const std::uint32_t rva = nameRva;
            std::copy( name.begin(), name.end(), b.begin() + static_cast<std::ptrdiff_t>( At( rva ) ) );
            nameRva += static_cast<std::uint32_t>( name.size() + 1 );
            return rva;
        };
        for ( std::size_t i = 0; i < spec.Imports.size(); ++i )
        {
            const std::uint32_t rva = writeName( spec.Imports[i] );
            // A real descriptor also has thunk RVAs; the reader must not need them to find the name.
            Put32( b, At( kSectionRva ) + i * 20 + 12,
                   ( i == 0 && spec.BadNameRva != 0 ) ? spec.BadNameRva : rva );
            Put32( b, At( kSectionRva ) + i * 20 + 16, 0x1300 );
        }
        for ( std::size_t i = 0; i < spec.DelayImports.size(); ++i )
        {
            Put32( b, At( kDelayRva ) + i * 32, 1 ); // attributes: RVA-based
            Put32( b, At( kDelayRva ) + i * 32 + 4, writeName( spec.DelayImports[i] ) );
        }
        return b;
    }

    std::vector<std::string> Read( const PeSpec& spec )
    {
        const auto image  = MakePe( spec );
        auto       result = ReadPeImports( image );
        EXPECT_TRUE( result ) << ( result ? "" : result.GetError() );
        return result ? result.ExtractValue() : std::vector<std::string>{};
    }

    class TempDir
    {
    public:
        TempDir()
        {
            std::random_device rd;
            m_Path = fs::temp_directory_path() / ( "PeImports-" + std::to_string( rd() ) );
            fs::create_directories( m_Path / "crt" );
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all( m_Path, ec );
        }
        TempDir( const TempDir& )            = delete;
        TempDir& operator=( const TempDir& ) = delete;

        [[nodiscard]] fs::path Crt() const
        {
            return m_Path / "crt";
        }

        static fs::path Write( const fs::path& file, const PeSpec& spec )
        {
            const auto bytes = MakePe( spec );
            const auto written =
                 Common::Utils::FileSystem::WriteBytesToFileAtomic( file, std::as_bytes( std::span( bytes ) ) );
            EXPECT_TRUE( written ) << file.string() << ": " << ( written ? "" : written.GetError() );
            return file;
        }
        [[nodiscard]] fs::path Exe( const PeSpec& spec ) const
        {
            return Write( m_Path / "Runtime.exe", spec );
        }
        void Dll( const std::string& name, const PeSpec& spec ) const
        {
            Write( Crt() / name, spec );
        }

    private:
        fs::path m_Path;
    };

    std::vector<std::string> Names( const std::vector<fs::path>& paths )
    {
        std::vector<std::string> names;
        names.reserve( paths.size() );
        for ( const fs::path& p : paths )
            names.push_back( p.filename().string() );
        return names;
    }
} // namespace

TEST( PeImports, ReadsTheImportTableThenTheDelayLoadTable )
{
    const auto names = Read(
         { .Imports = { "KERNEL32.dll", "VCRUNTIME140.dll", "MSVCP140.dll" }, .DelayImports = { "dwmapi.dll" } } );
    EXPECT_EQ( names,
               ( std::vector<std::string>{ "KERNEL32.dll", "VCRUNTIME140.dll", "MSVCP140.dll", "dwmapi.dll" } ) );
}

TEST( PeImports, ReadsPe32AsWellAsPe32Plus )
{
    const auto names = Read( { .Imports = { "KERNEL32.dll", "USER32.dll" }, .Pe32 = true } );
    EXPECT_EQ( names, ( std::vector<std::string>{ "KERNEL32.dll", "USER32.dll" } ) );
}

TEST( PeImports, ANameInBothTablesIsListedOnceWhateverItsCase )
{
    const auto names = Read( { .Imports = { "KERNEL32.dll" }, .DelayImports = { "kernel32.DLL" } } );
    EXPECT_EQ( names, ( std::vector<std::string>{ "KERNEL32.dll" } ) );
}

TEST( PeImports, AnImageWithoutAnImportDirectoryImportsNothing )
{
    EXPECT_TRUE( Read( {} ).empty() );
}

TEST( PeImports, RefusesWhatIsNotAPeImage )
{
    std::vector<std::uint8_t> notPe( 256, 0 );
    EXPECT_FALSE( ReadPeImports( notPe ) );

    auto noSignature   = MakePe( { .Imports = { "KERNEL32.dll" } } );
    noSignature[0x40]  = 'X';
    const auto refused = ReadPeImports( noSignature );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "offset 64" ), std::string::npos ) << refused.GetError();

    auto truncated = MakePe( { .Imports = { "KERNEL32.dll" } } );
    truncated.resize( 0x100 ); // section table present, section bytes gone
    EXPECT_FALSE( ReadPeImports( truncated ) );
}

TEST( PeImports, ANameOutsideEverySectionIsAnErrorNotAShorterList )
{
    const auto image   = MakePe( { .Imports = { "KERNEL32.dll", "USER32.dll" }, .BadNameRva = 0x9000 } );
    const auto refused = ReadPeImports( image );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "0x9000" ), std::string::npos ) << refused.GetError();
}

TEST( AppLocalRuntime, ShipsWhatTheExeImportsAndWhatThoseImportAndNothingElse )
{
    const TempDir dir;
    dir.Dll( "vcruntime140.dll", { .Imports = { "KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll" } } );
    dir.Dll( "vcruntime140_1.dll", { .Imports = { "VCRUNTIME140.dll", "KERNEL32.dll" } } );
    dir.Dll( "msvcp140.dll", { .Imports = { "VCRUNTIME140.dll", "KERNEL32.dll" } } );
    dir.Dll( "concrt140.dll", { .Imports = { "KERNEL32.dll" } } ); // present in the redist, not imported
    // The exe does NOT import vcruntime140.dll directly here: it reaches the package through msvcp140.
    const fs::path exe = dir.Exe( { .Imports = { "KERNEL32.dll", "MSVCP140.dll", "VCRUNTIME140_1.dll" } } );

    const auto closure = AppLocalRuntimeClosure( exe, dir.Crt() );
    ASSERT_TRUE( closure ) << closure.GetError();
    EXPECT_EQ( Names( closure.GetValue() ),
               ( std::vector<std::string>{ "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll" } ) );
}

TEST( AppLocalRuntime, AStaticCrtExeShipsNothing )
{
    const TempDir dir;
    dir.Dll( "vcruntime140.dll", { .Imports = { "KERNEL32.dll" } } );
    const fs::path exe = dir.Exe( { .Imports = { "KERNEL32.dll", "USER32.dll" } } );

    const auto closure = AppLocalRuntimeClosure( exe, dir.Crt() );
    ASSERT_TRUE( closure ) << closure.GetError();
    EXPECT_TRUE( closure.GetValue().empty() );
}

TEST( AppLocalRuntime, TheDebugCrtIsRefusedByName )
{
    const TempDir dir;
    dir.Dll( "vcruntime140.dll", { .Imports = { "KERNEL32.dll" } } );
    dir.Dll( "msvcp140.dll", { .Imports = { "KERNEL32.dll" } } );
    const fs::path exe =
         dir.Exe( { .Imports = { "KERNEL32.dll", "MSVCP140D.dll", "VCRUNTIME140D.dll", "ucrtbased.dll" } } );

    const auto closure = AppLocalRuntimeClosure( exe, dir.Crt() );
    ASSERT_FALSE( closure );
    for ( const char* name : { "MSVCP140D.dll", "VCRUNTIME140D.dll", "ucrtbased.dll", "Release or Shipping" } )
        EXPECT_NE( closure.GetError().find( name ), std::string::npos )
             << name << " not in: " << closure.GetError();
}

TEST( AppLocalRuntime, AnExeThatImportsNothingIsAReaderFailure )
{
    const TempDir dir;
    dir.Dll( "vcruntime140.dll", { .Imports = { "KERNEL32.dll" } } );
    const fs::path exe = dir.Exe( {} );
    EXPECT_FALSE( AppLocalRuntimeClosure( exe, dir.Crt() ) );
}

TEST( AppLocalRuntime, AnEmptyOrMissingRedistDirectoryIsRefused )
{
    const TempDir  dir;
    const fs::path exe = dir.Exe( { .Imports = { "KERNEL32.dll", "VCRUNTIME140.dll" } } );
    EXPECT_FALSE( AppLocalRuntimeClosure( exe, dir.Crt() ) );
    EXPECT_FALSE( AppLocalRuntimeClosure( exe, dir.Crt() / "absent" ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

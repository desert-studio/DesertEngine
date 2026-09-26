#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PeImports.hpp>

#include "../../TestSupport/pe_image.hpp"

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
    // The PE writer itself lives in TestSupport so PackagedContent can stage a runtime with the same
    // one; what stays here is everything that turns an image into an EXPECTATION.
    using Desert::TestSupport::MakePe;
    using Desert::TestSupport::PeSpec;

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

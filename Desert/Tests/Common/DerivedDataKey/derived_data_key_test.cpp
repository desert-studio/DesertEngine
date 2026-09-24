// AF5 — the DerivedDataCache's key, layout and boundaries.
//
// The key rule is a RELATION between inputs and one number, so each input gets its own mutation below:
// a setting, the deriver's version GUID (both halves), the payload — each must move the key — and the
// asset's path, which must NOT, because a move/rename re-deriving everything is exactly what a content
// key exists to prevent. The censuses read the repository as text: no cache path is spelled outside the
// DDC module, nothing but the packager names the Saved/Cooked tree, and git is told to ignore the DDC.

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr Common::DDC::Deriver kTestDeriver{
         "TestBucket", ".bin", { 0x1111222233334444ULL, 0x5555666677778888ULL } };

    fs::path RepoRoot()
    {
        for ( fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path() )
        {
            std::error_code ec;
            if ( fs::exists( dir / ".gitignore", ec ) && fs::is_directory( dir / "Desert" / "Common", ec ) )
                return dir;
            if ( dir == dir.parent_path() )
                break;
        }
        return {};
    }

    std::string ReadText( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    uint64_t KeyOfFile( const fs::path& file, const std::vector<uint32_t>& settings )
    {
        const std::string bytes = ReadText( file );
        return Common::DDC::MakeKey( kTestDeriver, Common::Utils::PakContentHash( bytes.data(), bytes.size() ),
                                     settings.data(), settings.size() * sizeof( uint32_t ) );
    }
} // namespace

TEST( DerivedDataKey, KeyMovesWithEverySettingAndTheDeriverVersion )
{
    const uint64_t                payload  = Common::Utils::PakContentHash( "payload", 7 );
    const std::array<uint32_t, 3> settings = { 64, 2, 1024 };
    const uint64_t base = Common::DDC::MakeKey( kTestDeriver, payload, settings.data(), sizeof( settings ) );

    EXPECT_EQ( base, Common::DDC::MakeKey( kTestDeriver, payload, settings.data(), sizeof( settings ) ) )
         << "the key must be a pure function of its inputs";

    for ( size_t i = 0; i < settings.size(); ++i )
    {
        auto changed = settings;
        changed[i] += 1;
        EXPECT_NE( base, Common::DDC::MakeKey( kTestDeriver, payload, changed.data(), sizeof( changed ) ) )
             << "setting #" << i << " changed and the key did not";
    }

    constexpr Common::DDC::Deriver bumpedHi{
         "TestBucket", ".bin", { 0x1111222233334445ULL, 0x5555666677778888ULL } };
    constexpr Common::DDC::Deriver bumpedLo{
         "TestBucket", ".bin", { 0x1111222233334444ULL, 0x5555666677778889ULL } };
    EXPECT_NE( base, Common::DDC::MakeKey( bumpedHi, payload, settings.data(), sizeof( settings ) ) )
         << "a new deriver GUID (high half) must invalidate every entry";
    EXPECT_NE( base, Common::DDC::MakeKey( bumpedLo, payload, settings.data(), sizeof( settings ) ) )
         << "a new deriver GUID (low half) must invalidate every entry";

    constexpr Common::DDC::Deriver otherBucket{
         "OtherBucket", ".bin", { 0x1111222233334444ULL, 0x5555666677778888ULL } };
    EXPECT_NE( base, Common::DDC::MakeKey( otherBucket, payload, settings.data(), sizeof( settings ) ) );

    EXPECT_NE( base, Common::DDC::MakeKey( kTestDeriver, payload + 1, settings.data(), sizeof( settings ) ) )
         << "different source bytes must be a different entry";
    EXPECT_NE( base, Common::DDC::MakeKey( kTestDeriver, payload, nullptr, 0 ) ) << "dropping the settings";
}

TEST( DerivedDataKey, KeyDoesNotMoveWithTheSourcePath )
{
    const fs::path  dir = fs::temp_directory_path() / "desert_ddc_path_invariance";
    std::error_code ec;
    fs::remove_all( dir, ec );
    fs::create_directories( dir / "Moved" / "Deeper", ec );
    const std::string bytes = "the same asset, twice";
    for ( const fs::path& p : { dir / "Original.ttf", dir / "Moved" / "Deeper" / "Renamed.ttf" } )
        std::ofstream( p, std::ios::binary ) << bytes;

    const std::vector<uint32_t> settings = { 48, 4 };
    EXPECT_EQ( KeyOfFile( dir / "Original.ttf", settings ),
               KeyOfFile( dir / "Moved" / "Deeper" / "Renamed.ttf", settings ) )
         << "moving or renaming an asset must hit the same DDC entry";
    fs::remove_all( dir, ec );
}

TEST( DerivedDataKey, LayoutIsUEBucketFanOut )
{
    const fs::path rel = Common::DDC::RelativePath( kTestDeriver, 0x0123456789abcdefULL );
    EXPECT_EQ( rel.generic_string(), "Buckets/TestBucket/01/23/456789abcdef.bin" );

    static_assert( Common::DDC::IsValidBucketName( "ShaderCache" ) );
    static_assert( !Common::DDC::IsValidBucketName( "" ) );
    static_assert( !Common::DDC::IsValidBucketName( "../escape" ) );
    static_assert( !Common::DDC::IsValidBucketName( "with space" ) );
    static_assert( !Common::DDC::IsValidBucketName(
         std::string_view( "a123456789012345678901234567890123456789012345678901234567890123" ) ) );
}

TEST( DerivedDataKey, RootComesFromTheSettingAndDefaultsIntoTheProject )
{
    const fs::path project = fs::path( "/proj" );
    EXPECT_EQ( Common::DDC::ResolveRoot( "", project ), fs::path( "/proj/DerivedDataCache" ) );
    EXPECT_EQ( Common::DDC::ResolveRoot( "../Shared/DDC", project ), fs::path( "/Shared/DDC" ) );
#if !defined( _WIN32 )
    EXPECT_EQ( Common::DDC::ResolveRoot( "/fast/ddc", project ), fs::path( "/fast/ddc" ) );
#endif

    // The live root follows the machine setting — read, not cached.
    auto&             machine    = Common::Settings::MachineSettings::Get();
    const std::string previous   = machine.DerivedDataCachePath;
    machine.DerivedDataCachePath = "ElsewhereDDC";
    EXPECT_EQ( Common::DDC::Root().filename(), "ElsewhereDDC" );
    machine.DerivedDataCachePath = previous;
}

TEST( DerivedDataKey, PutThenGetAndAPackagedGameLooksUnderCooked )
{
    const fs::path  project = fs::temp_directory_path() / "desert_ddc_roundtrip";
    std::error_code ec;
    fs::remove_all( project, ec );
    Common::Constants::Path::SetProjectRoot( project, "Assets" );

    EXPECT_FALSE( Common::DDC::Get( kTestDeriver, 42 ).has_value() ) << "an empty DDC is a miss, not an error";
    ASSERT_TRUE( Common::DDC::Put( kTestDeriver, 42, "derived bytes" ) );
    EXPECT_TRUE(
         fs::is_regular_file( project / "DerivedDataCache" / Common::DDC::RelativePath( kTestDeriver, 42 ) ) );
    EXPECT_EQ( Common::DDC::Get( kTestDeriver, 42 ).value_or( "" ), "derived bytes" );

    fs::remove_all( project / "DerivedDataCache", ec );
    EXPECT_FALSE( Common::DDC::Get( kTestDeriver, 42 ).has_value() ) << "deleting the DDC must be a miss";

    const fs::path loose = Common::DDC::PathFor( kTestDeriver, 42 );
    EXPECT_EQ( Common::DDC::PackagedPath( loose ).lexically_normal(),
               ( Common::Constants::Path::COOKED_PATH / Common::DDC::RelativePath( kTestDeriver, 42 ) )
                    .lexically_normal() );
    EXPECT_EQ( Common::DDC::PackagedPath( project / "Other.txt" ), project / "Other.txt" );

    EXPECT_EQ( Common::DDC::PlatformCookedDir(),
               ( project / "Saved" / "Cooked" / Common::DDC::CookPlatformName() ).lexically_normal() );
    Common::Constants::Path::ResetToSandbox();
    fs::remove_all( project, ec );
}

// ---- Censuses over the repository's text ------------------------------------------------------------

namespace
{
    std::vector<fs::path> SourcesUnder( const fs::path& root )
    {
        std::vector<fs::path> out;
        std::error_code       ec;
        for ( const auto& e : fs::recursive_directory_iterator( root, ec ) )
        {
            const auto ext = e.path().extension();
            if ( e.is_regular_file( ec ) && ( ext == ".cpp" || ext == ".hpp" || ext == ".h" ) )
                out.push_back( e.path() );
        }
        return out;
    }

    const std::array<const char*, 3> kEngineSourceRoots = { "Desert/Desert/Source", "Editor/Source",
                                                            "Runtime/Source" };
} // namespace

TEST( DerivedDataKey, OnlyTheCookersNameSavedCooked )
{
    // Saved/Cooked/<Platform> is WRITTEN by the two cookers and read by nothing in the editor or engine:
    // the packager (Editor/Packaging) and AssetRegistryTool's `cook`, which writes the gathered registry
    // there for a build that packs without the editor. Tools/ is scanned too, so a third tool that starts
    // writing (or reading) the cook output is a row added here on purpose, not a silent new consumer.
    const std::array<const char*, 2> kCookers = { "Editor/Source/Editor/Packaging/", "Tools/AssetRegistryTool/" };
    const fs::path                   repo     = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    size_t                   scanned = 0;
    std::array<size_t, 2>    seen{};
    std::vector<std::string> roots( kEngineSourceRoots.begin(), kEngineSourceRoots.end() );
    roots.emplace_back( "Tools" );
    for ( const std::string& root : roots )
        for ( const fs::path& file : SourcesUnder( repo / root ) )
        {
            ++scanned;
            const std::string text  = ReadText( file );
            const bool        names = text.find( "PlatformCookedDir" ) != std::string::npos ||
                               text.find( "Saved/Cooked" ) != std::string::npos;
            if ( !names )
                continue;
            const std::string rel    = file.lexically_relative( repo ).generic_string();
            bool              cooker = false;
            for ( size_t i = 0; i < kCookers.size(); ++i )
                if ( rel.rfind( kCookers[i], 0 ) == 0 )
                {
                    cooker = true;
                    ++seen[i];
                }
            EXPECT_TRUE( cooker ) << rel
                                  << " names the cook output; only the cookers write Saved/Cooked, the editor and "
                                     "the engine never read it";
        }
    EXPECT_GT( scanned, 500u ) << "the census read almost nothing — wrong root";
    for ( size_t i = 0; i < kCookers.size(); ++i )
        EXPECT_GT( seen[i], 0u ) << kCookers[i] << " no longer names the cook output — drop its row";
}

TEST( DerivedDataKey, NoDerivedCacheIsSpelledUnderTheCookedTree )
{
    // The six caches moved into the DDC. A path that joins COOKED_PATH with one of their names is the
    // old home coming back, and the entries would land in the tree a package ships wholesale.
    const std::array<const char*, 6> caches = { "ShaderCache", "PipelineCache",    "FontCache",
                                                "IconCache",   "EnvironmentCache", "Thumbnails" };
    const fs::path                   repo   = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    for ( const char* root : kEngineSourceRoots )
        for ( const fs::path& file : SourcesUnder( repo / root ) )
        {
            std::istringstream lines( ReadText( file ) );
            std::string        line;
            int                number = 0;
            while ( std::getline( lines, line ) )
            {
                ++number;
                if ( line.find( "COOKED_PATH" ) == std::string::npos )
                    continue;
                for ( const char* cache : caches )
                    EXPECT_EQ( line.find( std::string( "\"" ) + cache ), std::string::npos )
                         << file.lexically_relative( repo ).generic_string() << ":" << number << " puts " << cache
                         << " under COOKED_PATH; derived data lives in Common::DDC";
            }
        }
}

TEST( DerivedDataKey, GitIgnoresTheCacheAndNeverReIncludesIt )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    std::istringstream lines( ReadText( repo / ".gitignore" ) );
    std::string        line;
    bool               ignored      = false;
    bool               savedIgnored = false;
    while ( std::getline( lines, line ) )
    {
        if ( line == "DerivedDataCache/" )
            ignored = true;
        if ( line == "Saved/" )
            savedIgnored = true;
        if ( !line.empty() && line.front() == '!' )
            EXPECT_EQ( line.find( "DerivedDataCache" ), std::string::npos ) << "re-included: " << line;
    }
    EXPECT_TRUE( ignored ) << ".gitignore must carry a bare `DerivedDataCache/` line";
    EXPECT_TRUE( savedIgnored ) << ".gitignore must carry a bare `Saved/` line (the packager's cook output)";

    // The two lines are unanchored, so they would also swallow SOURCE: a directory of either name inside
    // the source trees is ignored and `git add` skips it without a word (this suite's first name did).
    for ( const char* root : { "Desert", "Editor/Source", "Runtime", "Tools" } )
    {
        std::error_code ec;
        for ( const auto& e : fs::recursive_directory_iterator( repo / root, ec ) )
            if ( e.is_directory( ec ) )
                EXPECT_TRUE( e.path().filename() != "DerivedDataCache" && e.path().filename() != "Saved" )
                     << e.path().lexically_relative( repo ).generic_string() << " is ignored by .gitignore";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

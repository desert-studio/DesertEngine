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

#include <algorithm>
#include <array>
#include <fstream>
#include <regex>
#include <set>
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
        // Git for Windows checks .gitignore out with CRLF (core.autocrlf, see .gitattributes), and git
        // itself drops the trailing CR when it reads the patterns; compare the line as git sees it.
        if ( !line.empty() && line.back() == '\r' )
            line.pop_back();
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

// ---- PK3 - the register of buckets a package ships is the ONLY source of what StageCookedEntries -----
// ---- copies, and this census keeps it honest in both directions. ---------------------------------------
//
// kMeshDeriver's bucket ("StaticMesh") was a Common::DDC::Deriver like any other, but PackageCook.cpp's
// shipped-bucket list was a second, hand-written array that nobody re-derived when the mesh deriver was
// added - so a packaged game shipped every cooked texture, font and shader and no mesh render data. The
// fix moved that list into Engine/Assets/DDCShipRegister.hpp's kDDCBucketRegister, which
// ShippedDDCBuckets() (and therefore StageCookedEntries) reads; this census scans the engine tree for
// every Common::DDC::Deriver declaration and checks it against that register, so a deriver added without
// a row here fails a test instead of shipping an empty bucket.
namespace
{
    struct DeclaredDeriverBucket
    {
        std::string Bucket;
        std::string File; // repo-relative, for a failure message that names where to add the row
    };

    // Every `Common::DDC::Deriver k...{ "Bucket", ... }` in the engine tree, single- or multi-line (the
    // std::regex `\s` class matches the newline between the brace and the bucket's string literal, which
    // is exactly the shape MeshDerivedData.hpp's declaration uses).
    std::vector<DeclaredDeriverBucket> DeclaredDeriverBuckets( const fs::path& repo )
    {
        static const std::regex            kPattern( R"re(DDC::Deriver\s+\w+\s*\{\s*"([A-Za-z0-9_]+)")re" );
        std::vector<DeclaredDeriverBucket> out;
        std::vector<std::string>           roots( kEngineSourceRoots.begin(), kEngineSourceRoots.end() );
        roots.emplace_back( "Tools" );
        for ( const std::string& root : roots )
            for ( const fs::path& file : SourcesUnder( repo / root ) )
            {
                const std::string rel = file.lexically_relative( repo ).generic_string();
                // The suite's own test deriver (kTestDeriver, "TestBucket") is fixture noise, not a real
                // bucket a runtime or an editor tool reads - it never appears in any ship register.
                if ( rel.find( "Tests/" ) != std::string::npos )
                    continue;
                const std::string text = ReadText( file );
                std::smatch       m;
                auto              begin = text.cbegin();
                while ( std::regex_search( begin, text.cend(), m, kPattern ) )
                {
                    out.push_back( { m[1].str(), rel } );
                    begin = m.suffix().first;
                }
            }
        return out;
    }

    struct RegisteredBucket
    {
        std::string Bucket;
        bool        Shipped; // false == DDCBucketReach::EditorOnly
        bool        HasReason;
    };

    // The register itself, read as text - this suite links only Common, so it cannot call
    // Desert::Assets::ShippedDDCBuckets() directly; reading the same header PackageCook.cpp compiles is
    // exactly the relation this census exists to check (the register's TEXT, not a copy of its intent).
    std::vector<RegisteredBucket> ReadDDCBucketRegister( const fs::path& repo )
    {
        static const std::regex kRow(
             R"re(\{\s*"([A-Za-z0-9_]+)"\s*,\s*DDCBucketReach::(Shipped|EditorOnly)\s*,\s*"([^"]+)")re" );
        const std::string text = ReadText( repo / "Desert/Desert/Source/Engine/Assets/DDCShipRegister.hpp" );
        std::vector<RegisteredBucket> out;
        std::smatch                   m;
        auto                          begin = text.cbegin();
        while ( std::regex_search( begin, text.cend(), m, kRow ) )
        {
            out.push_back( { m[1].str(), m[2].str() == "Shipped", !m[3].str().empty() } );
            begin = m.suffix().first;
        }
        return out;
    }

    // The only buckets the DDC holds with no Common::DDC::Deriver at all (BucketDir() called on a bare
    // string) - so DeclaredDeriverBuckets() cannot see them, and they are named here once, by hand, as
    // the one list this whole census cannot derive.
    const std::set<std::string> kNoDeriverEditorOnlyBuckets = { "Thumbnails" };
} // namespace

TEST( DerivedDataKey, EveryDDCDeriverBucketHasExactlyOneRegisterRow )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );

    const std::vector<DeclaredDeriverBucket> declared = DeclaredDeriverBuckets( repo );
    ASSERT_GE( declared.size(), 5u ) << "the census found almost no Deriver declarations - the regex or "
                                        "the search roots are broken, not the engine";

    const std::vector<RegisteredBucket> registered = ReadDDCBucketRegister( repo );
    ASSERT_FALSE( registered.empty() ) << "DDCShipRegister.hpp was not read - path moved?";

    std::set<std::string> registeredNames;
    for ( const RegisteredBucket& row : registered )
        registeredNames.insert( row.Bucket );

    // Direction 1: every deriver the engine declares owes exactly one row. A bucket declared twice (two
    // derivers sharing a name) or missing entirely both fail here - this is the direction that would
    // have caught "StaticMesh" before a player ever saw an empty mesh.
    for ( const DeclaredDeriverBucket& d : declared )
    {
        const size_t count = std::count_if( registered.begin(), registered.end(),
                                            [&]( const RegisteredBucket& r ) { return r.Bucket == d.Bucket; } );
        EXPECT_EQ( count, 1u ) << d.File << " declares a Common::DDC::Deriver for bucket \"" << d.Bucket
                               << "\", which has " << count
                               << " row(s) in DDCShipRegister.hpp's kDDCBucketRegister - add exactly one "
                                  "(Shipped or EditorOnly, with a reason) or the package silently drops it.";
    }

    // Direction 2: every row names a bucket that actually exists - either a declared deriver, or one of
    // the explicit no-deriver editor-only buckets. A row for a deriver that was renamed or removed is a
    // stale entry the register would otherwise carry forever.
    std::set<std::string> declaredNames;
    for ( const DeclaredDeriverBucket& d : declared )
        declaredNames.insert( d.Bucket );
    for ( const RegisteredBucket& row : registered )
        EXPECT_TRUE( declaredNames.count( row.Bucket ) == 1 ||
                     kNoDeriverEditorOnlyBuckets.count( row.Bucket ) == 1 )
             << "DDCShipRegister.hpp registers \"" << row.Bucket
             << "\", which names no Common::DDC::Deriver in the engine and is not in the explicit "
                "no-deriver editor-only allowlist - stale row or a typo.";

    // The totals are DERIVED from the two scans, not written as a literal count: this assertion cannot
    // drift out of sync with either side because it has no number of its own.
    EXPECT_EQ( registeredNames.size(), declaredNames.size() + kNoDeriverEditorOnlyBuckets.size() )
         << "the register's row count no longer matches (declared derivers) + (explicit no-deriver "
            "editor-only buckets) - one of the two directions above should also be failing.";
}

TEST( DerivedDataKey, EveryEditorOnlyBucketIsNamedExplicitlyAndJustified )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const std::vector<RegisteredBucket> registered = ReadDDCBucketRegister( repo );
    ASSERT_FALSE( registered.empty() );

    std::set<std::string> editorOnly;
    for ( const RegisteredBucket& row : registered )
    {
        if ( row.Shipped )
            continue;
        editorOnly.insert( row.Bucket );
        EXPECT_TRUE( row.HasReason ) << row.Bucket
                                     << " is marked EditorOnly with no reason string - an "
                                        "exclusion a reader cannot see the justification for is exactly "
                                        "the accident this row exists to rule out.";
    }

    // Named here, once, by hand - this IS the explicit list a reviewer checks; a bucket added to it
    // without a matching edit to this test is exactly the silent omission the brief asked not to allow.
    EXPECT_EQ( editorOnly, std::set<std::string>{ "Thumbnails" } )
         << "the set of EditorOnly buckets changed - update this test's expectation and say why the new "
            "bucket has no runtime reader.";
}

TEST( DerivedDataKey, PackageCookDerivesItsShippedBucketsFromTheRegisterNotALiteralList )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const std::string cook = ReadText( repo / "Editor/Source/Editor/Packaging/PackageCook.cpp" );
    ASSERT_FALSE( cook.empty() );

    EXPECT_NE( cook.find( "ShippedDDCBuckets" ), std::string::npos )
         << "PackageCook.cpp no longer calls Assets::ShippedDDCBuckets() - StageCookedEntries went back "
            "to a hand-written list, the exact structure that missed \"StaticMesh\" the first time.";

    const std::vector<RegisteredBucket> registered = ReadDDCBucketRegister( repo );
    ASSERT_FALSE( registered.empty() );
    for ( const RegisteredBucket& row : registered )
        if ( row.Shipped )
            EXPECT_EQ( cook.find( "\"" + row.Bucket + "\"" ), std::string::npos )
                 << "PackageCook.cpp still spells the bucket \"" << row.Bucket
                 << "\" as a literal - it should name only Assets::ShippedDDCBuckets() and let the "
                    "register be the one place that string is written.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

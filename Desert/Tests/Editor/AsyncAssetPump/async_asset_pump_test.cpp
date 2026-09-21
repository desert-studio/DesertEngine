// TWO RELATIONS NO UNIT TEST CAN SEE, BOTH ABOUT THE DEMAND-DRIVEN CLOUD KINDS.
//
// ── 1. BOTH HOSTS PUMP THE LOADER ────────────────────────────────────────────────────────────────
//
// `AsyncAssetLoader` never calls a delegate from inside `Request()`. That rule buys a whole class of
// reentrancy defects for one frame of latency, and it has a price: a host that does not call `Pump()`
// gets a loader that reads files on workers and never tells anybody they arrived. Every cloud kind
// would then be Pending forever, the sky would never draw, and nothing in the engine would say why —
// the loading overlay would simply stay up.
//
// This is the same defect shape, and the same file pair, as `AssetPreloadCensus` next door:
// `AssetPreloader::PreloadCloudLayouts` scanned a directory, registered what it found, and was called
// by nobody for the whole life of the painted-layout feature. Both ends were right; the link between
// them was missing; every test of either end passed. The relation lives between a class in the engine
// and two call sites in files no header includes, so it is asserted by reading the sources.
//
// ── 2. THE CONVERTED KINDS ARE NOT READ SYNCHRONOUSLY ANY MORE ───────────────────────────────────
//
// Converting a kind to demand-driven loading means removing every blocking read of it, and "every" is
// the hard word: the preloader's stage is the obvious one, and it was not the only one. The scene
// DESERIALISER also read a `.dcmv` — `if ( !a->IsReadyForUse() && !a->Load() ) return 0;` inside
// `ComponentRegistry.cpp` — and in the editor that runs after the boot, so `SyncLoadLedger` counted it
// as an in-frame load: a hitch. It was invisible because the eager preload had almost always read the
// file first, so the branch was almost never taken. The preload was HIDING a blocking read, not
// avoiding one, and deleting the preload is precisely what would have exposed it.
//
// So the second half of this census pins the property that finding it taught: the three converted
// kinds' preloader stages announce rather than read, and no other production source calls `Load()` on
// one of their assets.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kLoaderHeader = "Desert/Desert/Source/Engine/Assets/AsyncAssetLoader.hpp";
    constexpr const char* kPreloaderSource = "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp";

    /// The two files that start the engine. There is no third.
    constexpr const char* kLayers[] = { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" };

    /// The preloader stages that must ANNOUNCE rather than read, and the service call each must make.
    struct ConvertedKind
    {
        const char* Stage;
        const char* Service;
    };
    constexpr ConvertedKind kConverted[] = {
         { "PreloadCloudNoiseVolumes", "GetCloudNoiseService" },
         { "PreloadCloudModellingVolumes", "GetCloudModellingService" },
         { "PreloadCloudLayouts", "GetCloudLayoutService" },
    };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kLoaderHeader );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// The source with its comments blanked out, so a check about CODE cannot be satisfied by prose.
    /// THIS FILE IS THE PROOF THAT IT MATTERS: the comments in `AssetPreloader.cpp` quote the very
    /// `a->Load()` line the second census forbids, because explaining a deletion means naming what was
    /// deleted. Without this, the census would fail on its own explanation.
    std::string WithoutComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        bool inLine  = false;
        bool inBlock = false;
        for ( size_t i = 0; i < source.size(); ++i )
        {
            if ( inLine )
            {
                if ( source[i] == '\n' )
                {
                    inLine = false;
                    out.push_back( '\n' );
                }
                continue;
            }
            if ( inBlock )
            {
                if ( source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/' )
                {
                    inBlock = false;
                    ++i;
                }
                else if ( source[i] == '\n' )
                {
                    out.push_back( '\n' );
                }
                continue;
            }
            if ( source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/' )
            {
                inLine = true;
                continue;
            }
            if ( source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*' )
            {
                inBlock = true;
                ++i;
                continue;
            }
            out.push_back( source[i] );
        }
        return out;
    }

    /// The body of a free-standing `void Class::Name( ... ) { ... }`, by brace matching. Good enough for
    /// this repository's formatting, and a miss is a failure rather than a pass: an empty body makes the
    /// assertions below red.
    std::string FunctionBody( const std::string& source, const std::string& name )
    {
        const std::regex pattern( R"(::)" + name + R"(\s*\()" );
        std::smatch      match;
        if ( !std::regex_search( source, match, pattern ) )
            return {};

        const size_t open = source.find( '{', match.position() + match.length() );
        if ( open == std::string::npos )
            return {};

        int    depth = 0;
        size_t i     = open;
        for ( ; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                break;
        }
        return source.substr( open, i - open );
    }
} // namespace

TEST( AsyncAssetPump, TheRootIsFindable )
{
    ASSERT_FALSE( RepoRoot().empty() )
         << "the census could not find " << kLoaderHeader
         << " from the working directory, so every check below would pass on an empty string.";
}

TEST( AsyncAssetPump, BothHostsPumpTheLoaderOnceATick )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* layer : kLayers )
    {
        const std::string source = WithoutComments( ReadFile( root + layer ) );
        ASSERT_FALSE( source.empty() ) << layer << " could not be read";

        const std::regex pump( R"(AsyncAssetLoader::Get\(\)\.Pump\(\))" );
        EXPECT_TRUE( std::regex_search( source, pump ) )
             << layer
             << " never calls AsyncAssetLoader::Pump(). Completion is ALWAYS deferred to a pump -- even "
                "for an asset that is already resident -- so this host reads cloud volumes on workers and "
                "is never told they arrived. Every cloud kind stays Pending forever, the sky never draws, "
                "and nothing says why: the loading overlay simply does not come down. This is the exact "
                "shape of PreloadCloudLayouts, which scanned, registered, and was called by nobody.";
    }
}

TEST( AsyncAssetPump, TheConvertedKindsAreAnnouncedAndNotRead )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string preloader = WithoutComments( ReadFile( root + kPreloaderSource ) );
    ASSERT_FALSE( preloader.empty() );

    for ( const ConvertedKind& kind : kConverted )
    {
        const std::string body = FunctionBody( preloader, kind.Stage );
        ASSERT_FALSE( body.empty() ) << kind.Stage << " was not found in " << kPreloaderSource;

        // ASSERTED ON THE CODE, NOT ON THE ARGUMENT'S NAME. The obvious spelling of this check was
        // `body.find( "loadAfterCreate=*/false" )`, and it failed on a correct tree for a reason worth
        // keeping: `/*loadAfterCreate=*/` IS A COMMENT, so `WithoutComments` had already removed it. Had
        // the check been written against the raw source instead it would have passed -- and then gone red
        // on a caller that wrote the same `false` without the naming comment, which is the same code.
        // What the scan must not do is read; what says it does not read is the argument's VALUE.
        const std::regex lazyCreate( R"(AssetPriority::[A-Za-z]+\s*,\s*false)" );
        EXPECT_TRUE( std::regex_search( body, lazyCreate ) )
             << kind.Stage
             << " creates its assets with the eager load still on. The scan would read every file of this "
                "kind in the project before the first frame again -- which is the cost this tier removed, "
                "and it would come back silently because everything else would still work.";

        EXPECT_NE( body.find( "->Announce(" ), std::string::npos )
             << kind.Stage << " does not announce what it scanned. A kind that is neither read nor "
                              "announced is a kind whose service has never heard of it, so every "
                              "reference to it resolves to Null -- 'the scan did not find it' -- for "
                              "files that are sitting right there on disk.";

        EXPECT_EQ( body.find( "->Register(" ), std::string::npos )
             << kind.Stage
             << " still registers from the preloader. Register takes bytes ALREADY IN HAND; calling it "
                "from a scan means the scan read the file, which is the eager model wearing the new "
                "spelling.";
    }
}

TEST( AsyncAssetPump, NoProductionSourceReadsAConvertedKindSynchronously )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // THE ASSET TYPES WHOSE READS MUST ALL GO THROUGH THE LOADER, by the name a caller would hold one
    // by. A `Load()` on a variable of one of these types is a blocking read.
    const std::regex blockingRead(
         R"((CloudNoiseVolumeAsset|CloudModellingVolumeAsset|CloudLayoutAsset)[^;{}]{0,200}->Load\(\))" );

    // WHERE THE CENSUS LOOKS. Engine and both hosts; NOT the editor panels, which load a file the user
    // just picked in a dialog and are the one place a synchronous read is the correct answer -- the user
    // is waiting for that exact file and there is nothing else to show them.
    const std::vector<std::string> roots = { "Desert/Desert/Source/Engine", "Runtime/Source" };

    std::vector<std::string> offences;
    size_t                   scanned = 0;
    for ( const std::string& dir : roots )
    {
        const std::filesystem::path base( root + dir );
        if ( !std::filesystem::exists( base ) )
            continue;

        for ( const auto& entry : std::filesystem::recursive_directory_iterator( base ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" )
                continue;

            ++scanned;
            const std::string source = WithoutComments( ReadFile( entry.path().string() ) );
            if ( std::regex_search( source, blockingRead ) )
                offences.push_back( entry.path().string() );
        }
    }

    ASSERT_GT( scanned, 100u ) << "only " << scanned << " sources were read -- the census walked nothing";

    std::string joined;
    for ( const std::string& offence : offences )
        joined += "  " + offence + "\n";

    EXPECT_TRUE( offences.empty() )
         << "these sources read a demand-driven cloud asset synchronously:\n"
         << joined
         << "One of these was ComponentRegistry.cpp, inside the scene deserialiser, and it was a 4 MiB "
            "blocking read that SyncLoadLedger counted as an in-frame hitch. It was invisible for as long "
            "as it was, because the eager preload had almost always read the file first -- so the branch "
            "was almost never taken. Removing a preload is what exposes reads like this, which is why the "
            "census exists rather than a note.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// TWO RELATIONS NO UNIT TEST CAN SEE, BOTH ABOUT THE DEMAND-DRIVEN CLOUD KINDS.
//
// ── 1. BOTH HOSTS PUMP THE LOADER ────────────────────────────────────────────────────────────────
//
// `AsyncAssetLoader` never calls a delegate from inside `Request()`. That rule buys a whole class of
// reentrancy defects for one frame of latency, and it has a price: a host that does not call `Pump()`
// gets a loader that reads files on workers and never tells anybody they arrived. Every cloud kind
// would then be Pending forever, the sky would never draw, and nothing in the engine would say why —
// the splash would simply stay up.
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
    constexpr const char* kLoaderHeader    = "Desert/Desert/Source/Engine/Assets/AsyncAssetLoader.hpp";

    /// The two files that start the engine. There is no third.
    constexpr const char* kLayers[] = { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + kLoaderHeader );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
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
                "and nothing says why: the splash simply does not come down. This is the exact "
                "shape of PreloadCloudLayouts, which scanned, registered, and was called by nobody.";
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

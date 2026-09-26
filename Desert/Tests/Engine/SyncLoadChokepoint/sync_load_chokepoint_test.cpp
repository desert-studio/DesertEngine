// "EVERY SYNCHRONOUS ASSET LOAD PASSES THROUGH ONE TIMED POINT" — the behaviour, and the census that
// keeps the point from being walked around.
//
// WHAT WAS BROKEN, MEASURED ON `dev` @ 514cee91. `AssetBase::Load()` was pure virtual, and nothing
// counted a call to it. `Docs/World/GAP_ANALYSIS.md` §T0.1 calls a chokepoint here "the highest
// value-per-line item" in the whole document, because every claim in the tiers above it is otherwise
// unfalsifiable: `MeshService::Get` parses a cooked mesh on the frame that first touches it (40 185 992
// bytes of JSON for 105 317 vertices) and both "the preloader is eager, that is the cost" and "the
// preloader went lazy and the cost moved into the frame" produced exactly the same evidence — none.
//
// WHY THE POINT IS THE FUNCTION AND NOT A CALL SITE, counted on that same tree: `->Load()` / `.Load()`
// appears at 45 sites outside the tests (hot reload 6, the component registry 5, the cloud panels 8,
// `EnsureLoaded`, `CreateAsset`, and the editor's drag-and-drop and document-open paths). A detector at
// any one of them measures a fraction, and 45 hand-written scopes is what `ResourceLedger`'s header
// refuses by name: "thirteen places for a fourteenth to be forgotten".
//
// SO `Load()` IS NOW NON-VIRTUAL and takes the timing scope, and the per-type half is a protected
// `virtual LoadFromFile()`. The compiler closes the obvious route — `override` does not apply to a
// non-virtual member, so the old spelling does not build. It does NOT close the route of declaring a
// fresh `Load()` without `override`, which would shadow the base's silently and take 45 call sites with
// it. That is what the census below is for, and it is the only reason this file scans text at all.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Assets/SyncLoadLedger.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

namespace
{
    namespace fs = std::filesystem;
    using Desert::Assets::LoadPhase;
    using Desert::Assets::LoadTimingScope;
    using Desert::Assets::SyncLoadLedger;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Assets/AssetBase.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// Every header under Engine/Assets. The SET IS DERIVED from the tree rather than typed here, which
    /// is the whole difference between this census and a list: a nineteenth asset type is covered by
    /// existing, not by somebody remembering to add a line.
    std::vector<fs::path> AssetHeaders( const std::string& root )
    {
        std::vector<fs::path> out;
        std::error_code       ec;
        const fs::path        base = fs::path( root ) / "Desert/Desert/Source/Engine/Assets";
        for ( auto it = fs::recursive_directory_iterator( base, ec );
              !ec && it != fs::recursive_directory_iterator(); ++it )
        {
            if ( it->path().extension() == ".hpp" )
                out.push_back( it->path() );
        }
        return out;
    }

    /// Does this header declare a class whose base list names `AssetBase`? Deliberately crude — the
    /// question is "is this one of the types the chokepoint has to cover", and every asset type in the
    /// tree spells it `: public AssetBase`.
    bool DeclaresAnAssetSubclass( const std::string& text )
    {
        static const std::regex pattern( R"(:\s*public\s+AssetBase\b)" );
        return std::regex_search( text, pattern );
    }

    /// A slow enough body that the timer cannot round it to nothing on any machine. Sleeping rather than
    /// spinning: a spin loop is what an optimiser removes, and a timing test whose work was deleted
    /// passes by measuring zero.
    void Work( const int ms )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
    }

    struct LedgerFixture : ::testing::Test
    {
        void SetUp() override
        {
            SyncLoadLedger::ResetForTest();
        }
        void TearDown() override
        {
            SyncLoadLedger::ResetForTest();
        }
    };
    /// One message naming every offender. A FREE FUNCTION and not the `<< [&] { ... }()` lambda it
    /// replaces: a parameter-less multi-line lambda is the construct on which clang-format 18.1.3 (CI)
    /// and 18.1.8 (this machine) disagree, so the changed-lines gate can go red for code that is
    /// locally clean, and the repair people reach for is to hand-format until CI stops complaining.
    std::string Listing( const char* lead, const std::vector<std::string>& names )
    {
        std::string message = lead;
        for ( const std::string& name : names )
            message += "\n  " + name;
        return message;
    }
} // namespace

// ── THE BEHAVIOUR ──────────────────────────────────────────────────────────────────────────────────

TEST_F( LedgerFixture, AFreshLedgerIsInTheBootPhaseAndSaysSoRatherThanReportingZeroHitches )
{
    EXPECT_EQ( SyncLoadLedger::Phase(), LoadPhase::Boot );
    EXPECT_EQ( SyncLoadLedger::Loads(), 0u );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u );

    // "0 in-frame loads" on a process that has not started rendering is not the good news it looks
    // like, and the report has to distinguish it from the same number after a boot.
    const std::string report = SyncLoadLedger::Report();
    EXPECT_NE( report.find( "nothing loaded at all yet" ), std::string::npos ) << report;
}

TEST_F( LedgerFixture, OneScopeIsOneLoadAndItsTimeLandsInTheTotal )
{
    {
        const LoadTimingScope scope( "one.stmesh" );
        Work( 12 );
    }
    EXPECT_EQ( SyncLoadLedger::Loads(), 1u );
    EXPECT_GE( SyncLoadLedger::TotalMs(), 8.0 );
    EXPECT_EQ( SyncLoadLedger::SlowestPath(), "one.stmesh" );
}

TEST_F( LedgerFixture, ANESTEDLoadIsCountedTwiceAndTimedOnce )
{
    // THE RULE THAT KEEPS THE TOTAL UNDER WALL CLOCK. `CloudTypeAsset::Load()` reaches into the asset
    // manager mid-load to resolve the noise volume it names, and the prefab loader loads nested
    // prefabs. Adding every level's duration would report a boot that spent 250 % of its own duration
    // loading — a number a reader would reject, and therefore a detector nobody would use.
    // МЕРИМ ОТНОШЕНИЕ, А НЕ МИЛЛИСЕКУНДЫ. Прежде здесь стояли `>= 15.0` и `< 38.0`, и под санитайзером
    // верхняя граница ломалась: ASan замедляет и работу, и инструментацию, так что двадцать миллисекунд
    // работы приезжают тридцатью с лишним — свойство при этом держится, а тест краснеет. Красный,
    // который зависит от того, на чём его запустили, — это не проверка, а лотерея, и починить его
    // подъёмом порога значило бы подогнать число, чтобы гейт замолчал.
    //
    // Настоящее утверждение масштабонезависимо: внешняя область ПОКРЫВАЕТ внутреннюю, поэтому итог
    // обязан быть около стенных часов всего блока, а не их суммы. Обе величины замедляются одинаково.
    const auto wallStart = std::chrono::steady_clock::now();
    {
        const LoadTimingScope outer( "type.dcloudtype" );
        Work( 10 );
        {
            const LoadTimingScope inner( "volume.dcloudnoise" );
            Work( 10 );
        }
    }
    const double wallMs =
         std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - wallStart ).count();

    EXPECT_EQ( SyncLoadLedger::Loads(), 2u ) << "both loads are loads; the count is 'how many files'";

    // Нижняя граница — что мерили вообще: итог не может быть заметно меньше того, что заняло время.
    EXPECT_GE( SyncLoadLedger::TotalMs(), wallMs * 0.5 )
         << "итог меньше половины стенных часов — похоже, время не учли: " << SyncLoadLedger::Report();

    // Верхняя — что не сложили вложенное дважды. Сумма дала бы примерно ДВОЙНЫЕ стенные часы, поэтому
    // 1.5 отделяет «покрыто» от «сложено» при любом замедлении.
    EXPECT_LT( SyncLoadLedger::TotalMs(), wallMs * 1.5 )
         << "вложенное время сложено дважды (стенные часы " << wallMs << " мс): " << SyncLoadLedger::Report();
}

TEST_F( LedgerFixture, TheSlowestIsTrackedAtEVERYDepthBecauseTheFileWorthOpeningIsUsuallyTheInnerOne )
{
    {
        const LoadTimingScope outer( "prefab.dprefab" );
        {
            const LoadTimingScope inner( "huge.stmesh" );
            Work( 25 );
        }
    }
    // THE OUTER SCOPE IS LONGER IN WALL CLOCK AND MUST STILL LOSE. This assertion failed on the first
    // implementation, which ranked by wall time: the prefab won, as a container always does, and the
    // file that spent the 25 ms was never named. Ranking by SELF time — duration minus what the
    // children spent — is the fix, and it is why `LoadTimingScope` carries a parent pointer at all.
    EXPECT_EQ( SyncLoadLedger::SlowestPath(), "huge.stmesh" ) << SyncLoadLedger::Report();
    // And the self time it was ranked on is the inner scope's own, not the outer's.
    EXPECT_GE( SyncLoadLedger::SlowestMs(), 20.0 );
}

TEST_F( LedgerFixture, ALoadAfterTheBootIsCountedSeparatelyAndTheBootTotalIsNotContaminated )
{
    {
        const LoadTimingScope boot( "boot.stmesh" );
        Work( 10 );
    }
    ASSERT_EQ( SyncLoadLedger::InFrameLoads(), 0u );

    SyncLoadLedger::NoteBootFinished();
    ASSERT_EQ( SyncLoadLedger::Phase(), LoadPhase::Frame );

    {
        const LoadTimingScope hitch( "streamed.stmesh" );
        Work( 10 );
    }

    EXPECT_EQ( SyncLoadLedger::Loads(), 2u );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 1u );
    // The in-frame time is a SUBSET of the total, never the whole of it — the boot's own blocking reads
    // are legitimate and must not be summed into the hitch figure.
    EXPECT_GE( SyncLoadLedger::InFrameMs(), 8.0 );
    EXPECT_LT( SyncLoadLedger::InFrameMs(), SyncLoadLedger::TotalMs() );

    const std::string report = SyncLoadLedger::Report();
    EXPECT_NE( report.find( "in-frame: 1 load(s)" ), std::string::npos ) << report;
}

TEST_F( LedgerFixture, TheReportNamesTheSLOWESTInFrameLoadsNotTheFirstOnes )
{
    SyncLoadLedger::NoteBootFinished();

    // Twenty cheap in-frame loads and ONE expensive one placed past the per-load log cap: the case the
    // cap alone got wrong, because the load worth opening arrived after the sixteenth warning and was
    // only counted.
    constexpr int kCheap = 20;
    for ( int i = 0; i < kCheap; ++i )
    {
        if ( i == 18 )
        {
            const LoadTimingScope slow( "late_and_slow.stmesh" );
            Work( 15 );
        }
        const LoadTimingScope cheap( "cheap_" + std::to_string( i ) + ".stmesh" );
    }

    const std::string report = SyncLoadLedger::Report();
    const std::size_t header = report.find( "slowest in-frame load(s) by their OWN time (16 of 21):" );
    ASSERT_NE( header, std::string::npos ) << report;
    const std::size_t first = report.find( "\n    ", header );
    ASSERT_NE( first, std::string::npos ) << report;
    EXPECT_NE( report.substr( first, report.find( '\n', first + 1 ) - first ).find( "late_and_slow.stmesh" ),
               std::string::npos )
         << "the slowest in-frame load must be named first:\n"
         << report;
    EXPECT_NE( report.find( "the other 5 took at most" ), std::string::npos ) << report;
}

TEST_F( LedgerFixture, ZeroInFrameLoadsAfterABootIsPrintedAsARESULT )
{
    {
        const LoadTimingScope boot( "boot.stmesh" );
        Work( 2 );
    }
    SyncLoadLedger::NoteBootFinished();

    // "The preload caught everything" is the claim every later tier of the programme rests on. A report
    // that omitted the line when it was true would make the good case indistinguishable from a detector
    // that was not running — which is precisely the confusion this whole file exists to prevent.
    const std::string report = SyncLoadLedger::Report();
    EXPECT_NE( report.find( "every load so far happened at boot" ), std::string::npos ) << report;
}

TEST_F( LedgerFixture, TheBootPhaseIsOneWay )
{
    SyncLoadLedger::NoteBootFinished();
    SyncLoadLedger::NoteBootFinished();
    EXPECT_EQ( SyncLoadLedger::Phase(), LoadPhase::Frame )
         << "a process that has started playing cannot go back to booting";
}

TEST_F( LedgerFixture, AScopeThatUNWINDSStillRecords )
{
    // Eleven of the eighteen `LoadFromFile` bodies have at least one early return, and several return an
    // error. A pair of Begin/End calls would have been skipped by every one of them; a destructor cannot
    // be. Exercised through the path that skips the most code there is.
    // THE CATCH IS NOT EMPTY, and that is not only a lint: asserting the throw actually happened is
    // what stops this test passing on a body that never threw at all, which would exercise the ordinary
    // path under the name of the unwinding one.
    bool unwound = false;
    try
    {
        const LoadTimingScope scope( "throws.demat" );
        Work( 5 );
        throw std::runtime_error( "a load that gave up" );
    }
    catch ( const std::exception& thrown )
    {
        unwound = std::string( thrown.what() ) == "a load that gave up";
    }
    EXPECT_TRUE( unwound ) << "the throw this test is about did not happen";

    EXPECT_EQ( SyncLoadLedger::Loads(), 1u );
    EXPECT_GE( SyncLoadLedger::TotalMs(), 3.0 );
}

// ── THE CENSUS: THE POINT CANNOT BE WALKED AROUND ──────────────────────────────────────────────────

TEST( SyncLoadChokepointCensus, AssetBaseDeclaresLoadNonVirtuallyAndTakesTheTimingScopeInIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root";

    const std::string text = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Assets/AssetBase.hpp" ) );
    ASSERT_FALSE( text.empty() );

    EXPECT_EQ( text.find( "virtual Common::BoolResultStr Load()" ), std::string::npos )
         << "AssetBase::Load() is virtual again — every subclass can then take it over and the 45 call "
            "sites go back to being untimed";

    EXPECT_NE( text.find( "Common::BoolResultStr Load()" ), std::string::npos )
         << "AssetBase no longer declares Load() at all";

    EXPECT_NE( text.find( "virtual Common::BoolResultStr LoadFromFile() = 0" ), std::string::npos )
         << "the per-type half of the split is gone; there is nothing for a subclass to implement";

    // THE POINT HAS TO ACTUALLY TIME. A non-virtual `Load()` that forwarded without a scope would pass
    // every assertion above and count nothing — the shape of defect this repository has closed twelve
    // times: the comment promised a guarantee the line did not give.
    EXPECT_NE( text.find( "LoadTimingScope" ), std::string::npos )
         << "AssetBase::Load() no longer constructs a LoadTimingScope, so the chokepoint is a "
            "forwarding function that measures nothing";
}

TEST( SyncLoadChokepointCensus, NoAssetTypeDeclaresItsOwnLoad )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The route the compiler does NOT close: a fresh `Common::BoolResultStr Load()` with no `override`
    // shadows the base's and silently un-times all 45 call sites that go through a derived pointer.
    static const std::regex declaresLoad( R"(\bCommon::BoolResultStr\s+(\w+::)?Load\s*\()" );

    std::vector<std::string> offenders;
    for ( const fs::path& header : AssetHeaders( root ) )
    {
        if ( header.filename() == "AssetBase.hpp" )
            continue;
        const std::string text = Desert::Tests::ConsumerText::StripComments( ReadAll( header ) );
        if ( std::regex_search( text, declaresLoad ) )
            offenders.push_back( header.filename().string() );
    }

    EXPECT_TRUE( offenders.empty() ) << Listing(
         "these headers declare a Load() of their own, shadowing the timed one:", offenders );
}

TEST( SyncLoadChokepointCensus, EveryConcreteAssetTypeImplementsTheTimedHalf )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // DERIVED, NOT PINNED TO A COUNT. A gate that pins a NUMBER is satisfied by editing the number; the
    // set of types is whatever inherits AssetBase in the tree today, and each of them has to name the
    // function the chokepoint calls.
    //
    // WITH ONE EXEMPTION THE FIRST VERSION OF THIS TEST DID NOT HAVE, AND IT CAUGHT IT: `MeshAsset` and
    // `MaterialAsset` inherit AssetBase and correctly declare NOTHING — they are intermediate abstract
    // bases (`StaticMeshAsset`, `SkinnedMeshAsset` and `SurfaceMaterialAsset` derive from them and do
    // the reading). Demanding a `LoadFromFile` from them would have forced a body that cannot exist.
    // The exemption is DERIVED too: a type is intermediate when another asset header names it as a
    // base, so a real type that stopped implementing the function is not exempted by accident.
    std::vector<std::string>    subclasses;
    std::vector<std::string>    missing;
    const std::vector<fs::path> headers = AssetHeaders( root );
    std::vector<std::string>    allText;
    allText.reserve( headers.size() );
    for ( const fs::path& header : headers )
        allText.push_back( Desert::Tests::ConsumerText::StripComments( ReadAll( header ) ) );

    for ( std::size_t i = 0; i < headers.size(); ++i )
    {
        const std::string& text = allText[i];
        if ( !DeclaresAnAssetSubclass( text ) )
            continue;
        const std::string name = headers[i].stem().string();
        subclasses.push_back( name );
        if ( text.find( "LoadFromFile" ) != std::string::npos )
            continue;

        const std::regex derivesFromIt( R"(:\s*public\s+)" + name + R"(\b)" );
        bool             isABaseForSomebody = false;
        for ( std::size_t j = 0; j < headers.size(); ++j )
        {
            if ( j != i && std::regex_search( allText[j], derivesFromIt ) )
            {
                isABaseForSomebody = true;
                break;
            }
        }
        if ( !isABaseForSomebody )
            missing.push_back( name );
    }

    EXPECT_GE( subclasses.size(), 10u )
         << "the scan found only " << subclasses.size()
         << " AssetBase subclasses, which means the scan broke rather than that the tree shrank";
    EXPECT_TRUE( missing.empty() ) << Listing(
         "these concrete AssetBase subclasses never name LoadFromFile, so their loads are untimed:", missing );
}

TEST( SyncLoadChokepointCensus, BothHostsCloseTheirBootSoAnInFrameLoadCanBeRecognisedAtAll )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The phase is what separates a loading screen from a hitch, and it is set by the LAYER — nothing
    // else knows when a boot is over. A host that forgot this line would report every one of its
    // in-frame loads as a boot load, which is a detector that is on and always answers "fine".
    for ( const char* layer : { "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp" } )
    {
        const std::string text = Desert::Tests::ConsumerText::StripComments( ReadAll( fs::path( root ) / layer ) );
        ASSERT_FALSE( text.empty() ) << "could not read " << layer;
        EXPECT_NE( text.find( "SyncLoadLedger::NoteBootFinished" ), std::string::npos )
             << layer << " never closes its boot, so every load it makes afterwards counts as boot work";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

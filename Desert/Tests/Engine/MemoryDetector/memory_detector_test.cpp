// "THE MEMORY NUMBER HAS TO SPEAK ABOUT GROWTH, AND AN UNKNOWN MUST NOT LOOK LIKE A ZERO."
//
// WHAT WAS BROKEN, AND IT IS A MEASUREMENT, NOT AN OPINION. `ResourceLedger::Report()` was the only
// memory figure the engine printed by itself. Between the control scene and the 50 179-entity world
// scene (`Docs/World/WORLD_SCENE.md`) its byte total moved by 677 829 bytes — 0.15 % — while the process
// grew by 315 MB, 419.5 -> 734.6 MB of peak resident. The ledger was not wrong. It counts device
// objects, and fifty thousand cubes sharing one mesh add almost none. It was answering a question nobody
// had asked, and there was no second number to notice that.
//
// So the readout is three numbers now, and the assertions here are about the three things that can go
// wrong with three numbers rather than one:
//
//   1. a source that declined to answer must print as UNKNOWN, never as 0 — a 0 B resident process
//      reads as spectacularly small rather than as a failed syscall;
//   2. the ledger's total must carry its own coverage, because a subtotal presented as a total is the
//      original defect;
//   3. growth is measured against a BASELINE and a PEAK, because resident size falls under memory
//      pressure with nothing having been freed, so a single instantaneous reading cannot answer "did it
//      grow" in either direction.
//
// AND ONE CENSUS, because the most valuable property of this detector is not observable from inside it:
// the Vulkan specification calls `VK_EXT_memory_budget`'s numbers "a rough estimate" that is "not
// invariant", so a CACHED reading is not a cheaper measurement but a different and wrong one. Nothing a
// test can call proves the absence of a cache; the source text can.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Common/Utilities/ProcessMemory.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using Desert::Graphic::MemoryReadout;
    using Desert::Graphic::MemoryWatch;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/MemoryReadout.hpp" );
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

    MemoryReadout WithProcess( const uint64_t resident, const uint64_t peak )
    {
        MemoryReadout out;
        out.Process.Known    = true;
        out.Process.Resident = resident;
        out.Process.Peak     = peak;
        return out;
    }

    MemoryReadout WithDeviceUsage( const uint64_t usage, const uint64_t budget )
    {
        MemoryReadout out;
        out.Device.BudgetKnown = true;
        Desert::Engine::DeviceMemoryHeap heap;
        heap.DeviceLocal = true;
        heap.Size        = budget * 2;
        heap.Budget      = budget;
        heap.Usage       = usage;
        out.Device.Heaps.push_back( heap );
        return out;
    }

    struct WatchFixture : ::testing::Test
    {
        void SetUp() override
        {
            MemoryWatch::ResetForTest();
        }
        void TearDown() override
        {
            MemoryWatch::ResetForTest();
        }
    };
} // namespace

// ── 1. THE PROCESS FOOTPRINT: THE NUMBER THAT SAW THE 315 MB ───────────────────────────────────────

TEST( ProcessFootprint, ThisPlatformAnswers )
{
    // A NEGATIVE CONTROL OF ITS OWN KIND: if this suite ever runs on a platform the reader does not
    // implement, this fails loudly here instead of every growth number quietly becoming 0.
    const Common::Utils::ProcessFootprint footprint = Common::Utils::ReadProcessFootprint();
    ASSERT_TRUE( footprint.Known ) << "the platform query failed: " << footprint.Describe();
    EXPECT_GT( footprint.Resident, 0u );
    EXPECT_GE( footprint.Peak, footprint.Resident ) << "a high-water mark below the current value is not a "
                                                       "high-water mark";
}

TEST( ProcessFootprint, AnUnknownReadingSaysSoInsteadOfPrintingZeroBytes )
{
    const Common::Utils::ProcessFootprint unknown;
    ASSERT_FALSE( unknown.Known );
    const std::string text = unknown.Describe();
    EXPECT_NE( text.find( "unknown" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "resident=0" ), std::string::npos )
         << "a failed query printed as zero bytes, which reads as a tiny process: " << text;
}

TEST( ProcessFootprint, ItGrowsWhenTheProcessGrows )
{
    // THE ONE ASSERTION THAT PROVES THE READER IS WIRED TO REALITY rather than returning a plausible
    // constant. 64 MiB touched page by page, because an untouched allocation is not resident and a
    // reader of RSS is right to ignore it.
    const uint64_t before = Common::Utils::ReadProcessFootprint().Resident;

    constexpr std::size_t kBytes = std::size_t{ 64 } * 1024 * 1024;
    auto                  block  = std::make_unique<volatile unsigned char[]>( kBytes );
    for ( std::size_t at = 0; at < kBytes; at += 4096 )
        block[at] = static_cast<unsigned char>( at );

    const uint64_t after = Common::Utils::ReadProcessFootprint().Resident;
    EXPECT_GT( after, before + ( uint64_t{ 32 } * 1024 * 1024 ) )
         << "touching 64 MiB moved resident from " << before << " to " << after;
}

// ── 2. THE DEVICE READING: UNKNOWN IS NOT EMPTY ────────────────────────────────────────────────────

TEST( DeviceMemoryReport, ADeviceThatDeclinedToAnswerReportsUnknownRatherThanAnEmptyDevice )
{
    const MemoryReadout readout; // no device, as in every suite and during static initialisation
    ASSERT_FALSE( readout.Device.BudgetKnown );

    const std::string text = readout.Report();
    EXPECT_NE( text.find( "device=unknown" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "device-local usage=0" ), std::string::npos )
         << "an absent extension printed as zero usage, which reads as an idle GPU: " << text;
}

TEST( DeviceMemoryReport, OnlyDeviceLocalHeapsAreSummed )
{
    Desert::Engine::DeviceMemoryReport report;
    report.BudgetKnown = true;
    Desert::Engine::DeviceMemoryHeap local;
    local.DeviceLocal = true;
    local.Usage       = 700;
    local.Budget      = 1000;
    Desert::Engine::DeviceMemoryHeap host;
    host.DeviceLocal = false;
    host.Usage       = 90'000;
    host.Budget      = 99'000;
    report.Heaps     = { local, host };

    // A host-visible staging heap is not the budget the renderer spends against, and folding it in would
    // have hidden a device-local heap running out behind a system heap that never does.
    EXPECT_EQ( report.DeviceLocalUsage(), 700u );
    EXPECT_EQ( report.DeviceLocalBudget(), 1000u );
}

TEST( DeviceMemoryReport, TheBudgetIsReportedSeparatelyFromTheHeapSizeBecauseItIsSmaller )
{
    // MEASURED ON THIS MACHINE with a standalone probe against MoltenVK 1.1.357 / Apple M1 Pro: one heap
    // of 17 179 869 184 B whose budget reads 12 713 115 648 B. A detector that compared usage against the
    // heap SIZE would have reported 26 % of head-room that the implementation will not hand out.
    const MemoryReadout readout = WithDeviceUsage( 1'000'000, 12'713'115'648ull );
    ASSERT_FALSE( readout.Device.Heaps.empty() );
    EXPECT_LT( readout.Device.DeviceLocalBudget(), readout.Device.Heaps.front().Size );

    const std::string text = readout.Report();
    EXPECT_NE( text.find( "of budget=" ), std::string::npos ) << text;
}

// ── 3. THE LEDGER LINE: A SUBTOTAL IS LABELLED AS ONE ──────────────────────────────────────────────

TEST( MemoryReadoutReport, ThePartialLedgerTotalIsCalledAFloor )
{
    MemoryReadout readout;
    readout.Ledger.Live          = 591; // the count measured on the world scene
    readout.Ledger.BytesKnownFor = 39;  // and how many of them reported a size
    readout.Ledger.Bytes         = 677'829;

    const std::string text = readout.Report();
    EXPECT_NE( text.find( "39 of 591" ), std::string::npos ) << text;
    // The number that started this whole task was a subtotal over 39 of 591 rows, printed without a
    // qualifier, and read as the engine's memory. It is a floor, and it has to say so.
    EXPECT_NE( text.find( "FLOOR" ), std::string::npos ) << text;
}

TEST( MemoryReadoutReport, AFullyCoveredLedgerIsNotCalledAFloor )
{
    // The negative control. A qualifier that is always printed is not a qualifier.
    MemoryReadout readout;
    readout.Ledger.Live          = 12;
    readout.Ledger.BytesKnownFor = 12;
    readout.Ledger.Bytes         = 4096;
    EXPECT_EQ( readout.Report().find( "FLOOR" ), std::string::npos ) << readout.Report();
}

// ── 4. THE WATCH: GROWTH IS BASELINE TO PEAK ───────────────────────────────────────────────────────

TEST_F( WatchFixture, AWatchThatNeverSampledSaysSoRatherThanReportingNoGrowth )
{
    ASSERT_EQ( MemoryWatch::FramesSampled(), 0u );
    const std::string text = MemoryWatch::Report();
    EXPECT_NE( text.find( "never sampled" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "grew 0 B" ), std::string::npos )
         << "a detector that never ran reported that nothing grew: " << text;
}

TEST_F( WatchFixture, TheFirstSampleIsTheBaselineAndLaterOnesCannotMoveIt )
{
    MemoryWatch::SampleFrame( WithProcess( 100, 100 ) );
    MemoryWatch::SampleFrame( WithProcess( 500, 500 ) );
    MemoryWatch::SampleFrame( WithProcess( 300, 500 ) );

    EXPECT_EQ( MemoryWatch::FramesSampled(), 3u );
    EXPECT_EQ( MemoryWatch::BaselineProcessResident(), 100u )
         << "the origin of every growth number moved after the fact";
}

TEST_F( WatchFixture, ThePeakSurvivesAFALLINGReading )
{
    // THIS IS WHY THERE IS A PEAK AT ALL. Resident size drops under memory pressure with nothing having
    // been freed, so an instantaneous reading taken after the pressure describes a process that never
    // happened. The 315 MB figure this detector was built for is a peak-to-peak difference.
    MemoryWatch::SampleFrame( WithProcess( 419'500'000, 419'500'000 ) );
    MemoryWatch::SampleFrame( WithProcess( 734'600'000, 734'600'000 ) );
    MemoryWatch::SampleFrame( WithProcess( 420'000'000, 734'600'000 ) );

    EXPECT_EQ( MemoryWatch::PeakProcessResident(), 734'600'000u );

    const std::string text = MemoryWatch::Report();
    EXPECT_NE( text.find( "grew" ), std::string::npos ) << text;
    // PINNED ON THE EXACT BYTE COUNT, not the rounded megabytes: the rounding is presentation and the
    // difference is the measurement. 734 600 000 - 419 500 000 = 315 100 000, which is the 315 MB this
    // whole detector was built because nothing reported.
    EXPECT_NE( text.find( "grew 315100000 B" ), std::string::npos ) << text;
}

TEST_F( WatchFixture, ADeviceThatNeverAnsweredIsReportedAsUnknownAndNotAsZeroGrowth )
{
    MemoryWatch::SampleFrame( WithProcess( 100, 100 ) ); // no device half at all
    const std::string text = MemoryWatch::Report();
    EXPECT_NE( text.find( "device-local usage: unknown" ), std::string::npos ) << text;
}

TEST_F( WatchFixture, TheDeviceHalfIsTrackedIndependentlyOfTheProcessHalf )
{
    // The whole point of keeping three numbers: they move for different reasons, and a run where the
    // device grows and the process does not is a different diagnosis from the reverse.
    MemoryWatch::SampleFrame( WithDeviceUsage( 1'000, 10'000 ) );
    MemoryWatch::SampleFrame( WithDeviceUsage( 9'000, 10'000 ) );
    EXPECT_EQ( MemoryWatch::BaselineDeviceLocalUsage(), 1'000u );
    EXPECT_EQ( MemoryWatch::PeakDeviceLocalUsage(), 9'000u );
    EXPECT_EQ( MemoryWatch::PeakProcessResident(), 0u );
}

// ── 5. THE CENSUS: NOTHING CACHES THE READING, AND THE SHIPPED PATH TAKES IT ────────────────────────

TEST( MemoryDetectorCensus, TheExtensionIsEnabledOnTheDeviceAndGatesTheChainedStruct )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string text = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp" ) );
    ASSERT_FALSE( text.empty() );

    // THE LINE THAT DOES IT, NOT THE MENTION OF IT. The first version of this assertion looked for the
    // extension NAME anywhere in the file and SURVIVED a mutation that deleted the push_back — because
    // the `IsExtensionSupported( ... )` test above it still names the same constant. A rule satisfied by
    // a mention is satisfied by the `if` that decides nothing.
    EXPECT_NE( text.find( "deviceExtensions.push_back( VK_EXT_MEMORY_BUDGET_EXTENSION_NAME )" ),
               std::string::npos )
         << "the device is created without VK_EXT_memory_budget in its extension list, so every usage "
            "figure the driver reports is zero";

    // AND THE GATE. Chaining VkPhysicalDeviceMemoryBudgetPropertiesEXT into a query on a device that was
    // not created with the extension is undefined behaviour whose observed shape is a struct nobody
    // wrote to — zeros, which is the one wrong answer this readout exists to stop us believing. A
    // pNext assigned unconditionally would pass every behavioural test in this file.
    EXPECT_NE( text.find( "m_MemoryBudgetEnabled ?" ), std::string::npos )
         << "the budget struct is chained without checking that the extension was enabled";
}

TEST( MemoryDetectorCensus, NothingStoresTheReadingBetweenQueries )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // The specification's own words are "a rough estimate" that is "not invariant". A member holding the
    // last answer is how that estimate becomes a fact with a timestamp nobody can see — so the device
    // implementation must build its reply in a LOCAL and return it, with no `m_*Memory*Report`-shaped
    // member anywhere near it.
    const std::string vulkan = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.hpp" ) );
    ASSERT_FALSE( vulkan.empty() );
    EXPECT_EQ( vulkan.find( "m_MemoryReport" ), std::string::npos )
         << "the device caches a memory reading; the spec forbids treating it as invariant";
    EXPECT_EQ( vulkan.find( "DeviceMemoryReport m_" ), std::string::npos ) << "the device caches a reading";

    // And the interface must not offer a getter for "the last reading", which is the other way a cache
    // arrives — this time in the callers rather than in the backend.
    const std::string iface = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Core/Device.hpp" ) );
    ASSERT_FALSE( iface.empty() );
    EXPECT_EQ( iface.find( "LastMemory" ), std::string::npos );
    EXPECT_EQ( iface.find( "CachedMemory" ), std::string::npos );
}

TEST( MemoryDetectorCensus, TheShippedFramePathTakesAReadingEveryFrame )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // NOT A HUD, AND NOT BEHIND A FLAG. §0.4 of the world programme is about the host the player runs,
    // which draws no HUD at all — a detector sampled from an editor panel measures the editor. And
    // BeginFrame rather than EndFrame, for the reason the draw counters chose the same place: a frame
    // that loses the device is never recorded.
    const std::string renderer = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/Renderer.cpp" ) );
    ASSERT_FALSE( renderer.empty() );

    // BOUNDED BY THE FUNCTION'S OWN TWO LANDMARKS rather than by where `Renderer::EndFrame` happens to
    // sit in the file. The first spelling of this assertion compared against `EndFrame`'s position and
    // failed on a tree where `EndFrame` is DEFINED ABOVE `BeginFrame` — a green-looking rule that was
    // measuring the order of definitions, which nothing depends on.
    const std::size_t begin    = renderer.find( "Renderer::BeginFrame" );
    const std::size_t delegate = renderer.find( "s_RendererAPI->BeginFrame()" );
    const std::size_t sample   = renderer.find( "MemoryWatch::SampleFrame" );
    ASSERT_NE( begin, std::string::npos );
    ASSERT_NE( delegate, std::string::npos ) << "Renderer::BeginFrame no longer calls the backend";
    ASSERT_NE( sample, std::string::npos ) << "no frame path samples the memory watch";
    EXPECT_GT( sample, begin ) << "the sample sits above Renderer::BeginFrame, i.e. outside it";
    EXPECT_LT( sample, delegate )
         << "the sample is after the backend call, so a frame the backend refuses never contributes its "
            "reading — and the refused frame is the interesting one";

    // A FRESH READING, not a stored one handed back in: the call site is what says so.
    EXPECT_NE( renderer.find( "MemoryWatch::SampleFrame( MemoryReadout::TakeFrameSample() )" ), std::string::npos )
         << "the frame path no longer takes a fresh reading at the call site";
}

TEST( MemoryDetectorCensus, ThePerFrameSampleDoesNotWalkTheResourceLedger )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // `ResourceLedger`'s header states the constraint this asserts: "the ledger is deliberately NOT
    // consulted while drawing — it answers questions between frames". Its census is a locked traversal
    // of every live row, and the watch tracks neither field it fills — so a frame path that took the
    // full readout would be spending the frame budget it exists to measure. The first version of this
    // detector did exactly that.
    const std::string source = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/MemoryReadoutSource.cpp" ) );
    ASSERT_FALSE( source.empty() );

    const std::size_t frameSample = source.find( "MemoryReadout::TakeFrameSample" );
    ASSERT_NE( frameSample, std::string::npos ) << "there is no per-frame taker any more";
    EXPECT_EQ( source.find( "ResourceLedger::Take", frameSample ), std::string::npos )
         << "the per-frame sample walks the resource ledger under its mutex, once per frame";

    // And the full readout still does ask — the attribution half must not disappear with the fix.
    EXPECT_NE( source.find( "ResourceLedger::Take" ), std::string::npos )
         << "nothing takes the ledger census any more, so the report lost its attribution half";
    EXPECT_LT( source.find( "ResourceLedger::Take" ), frameSample );
}


// ── 6. WHOSE BYTES — THE CENSUS ATTRIBUTES DEVICE MEMORY, NOT ONLY OBJECT COUNTS ────────────────────
//
// WHAT WAS MISSING, AND WHICH DECISION IT BLOCKED. `Docs/World/PROGRAMME.md` §7 (streaming mip levels)
// is decided on one quantity: how much of a scene's device memory is texture. The ledger could name
// every owner and every kind and could report ONE total for all of them, so "AssetService holds one
// Image2D" was the whole answer — the same sentence whether that image is a 64x64 icon or a 2048x2048
// albedo with its mip chain, a factor of a thousand apart. The share therefore had to be worked out by
// hand from a texture's dimensions, outside the engine, which makes it an argument rather than a
// reading. The cell `(AssetService, Image2D)` is that reading.
//
// The three things that can go wrong with a table of bytes, one test each:
//   1. bytes land in the WRONG cell — a table indexed by two enumerators is exactly where that happens;
//   2. an owner total DISAGREES with the cells it is made of, which is what a stored total does the day
//      one of the two writers is forgotten;
//   3. a cell with rows and no known size reports 0 and reads as "this owner holds nothing".

namespace
{
    using Desert::Graphic::ResourceCensus;
    using Desert::Graphic::ResourceKind;
    using Desert::Graphic::ResourceLedger;
    using Desert::Graphic::ResourceOwner;
    using Desert::Graphic::ResourceOwnership;

    /// Deltas, never absolutes. The ledger is one map for the whole process and every other test in this
    /// binary may hold rows in it; an assertion on an absolute figure would be an assertion about test
    /// ORDER.
    uint64_t BytesDelta( const ResourceCensus& before, const ResourceCensus& after, const ResourceOwner owner,
                         const ResourceKind kind )
    {
        return after.BytesFor( owner, kind ) - before.BytesFor( owner, kind );
    }
} // namespace

TEST( ResourceCensusBytes, BytesLandInTheCellOfTheOwnerAndKindThatHoldsThem )
{
    const ResourceCensus before = ResourceLedger::Take();

    // The two rows the world scene actually has, with its measured sizes: one 1024x1024 RGBA8 texture
    // with its mip chain, and one renderer holding four 2048x2048 shadow cascades.
    auto texture = ResourceOwnership::Take( ResourceKind::Image2D, 5'592'405 );
    texture.Claim( ResourceOwner::AssetService );

    auto cascades = ResourceOwnership::Take( ResourceKind::Image2D, 335'544'320 );
    cascades.Claim( ResourceOwner::SceneRenderer );

    auto mesh = ResourceOwnership::Take( ResourceKind::VertexBuffer, 7'338'168 );
    mesh.Claim( ResourceOwner::AssetService );

    const ResourceCensus after = ResourceLedger::Take();

    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::AssetService, ResourceKind::Image2D ), 5'592'405u );
    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::SceneRenderer, ResourceKind::Image2D ),
               335'544'320u );
    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::AssetService, ResourceKind::VertexBuffer ),
               7'338'168u );

    // AND THE CELL THAT MUST NOT HAVE MOVED. Without this half the assertions above pass for an
    // implementation that adds every row's bytes to every cell.
    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::SceneRenderer, ResourceKind::VertexBuffer ), 0u );
    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::EditorTool, ResourceKind::Image2D ), 0u );
}

TEST( ResourceCensusBytes, ClosingTheRowTakesItsBytesWithIt )
{
    const ResourceCensus before = ResourceLedger::Take();
    {
        auto texture = ResourceOwnership::Take( ResourceKind::Image2D, 21'845'000 );
        texture.Claim( ResourceOwner::AssetService );
        const ResourceCensus during = ResourceLedger::Take();
        ASSERT_EQ( BytesDelta( before, during, ResourceOwner::AssetService, ResourceKind::Image2D ),
                   21'845'000u );
    }
    // A ledger that reports memory for objects that are gone drifts upward for ever and nothing can tell
    // that it has — the defect `ResourceLedger.hpp` refuses to allow, now asserted for the byte half too.
    const ResourceCensus after = ResourceLedger::Take();
    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::AssetService, ResourceKind::Image2D ), 0u );
}

TEST( ResourceCensusBytes, AnOwnerTotalIsExactlyTheSumOfItsOwnCells )
{
    const ResourceCensus before = ResourceLedger::Take();

    auto image  = ResourceOwnership::Take( ResourceKind::Image2D, 1'000 );
    auto buffer = ResourceOwnership::Take( ResourceKind::VertexBuffer, 200 );
    auto shader = ResourceOwnership::Take( ResourceKind::Shader, 30 );
    image.Claim( ResourceOwner::AssetService );
    buffer.Claim( ResourceOwner::AssetService );
    shader.Claim( ResourceOwner::AssetService );

    const ResourceCensus after = ResourceLedger::Take();

    EXPECT_EQ( after.BytesForOwner( ResourceOwner::AssetService ) -
                    before.BytesForOwner( ResourceOwner::AssetService ),
               1'230u );
    EXPECT_EQ( after.BytesKnownForOwner( ResourceOwner::AssetService ) -
                    before.BytesKnownForOwner( ResourceOwner::AssetService ),
               3u );

    // THE RELATION, not the two numbers separately. The owner total is DERIVED, so the only thing worth
    // asserting about it is that it equals the cells it is derived from — which is the property a stored
    // total loses the day somebody adds a row to the table and not to the total.
    uint64_t summed = 0;
    for ( std::size_t kind = 0; kind < static_cast<std::size_t>( ResourceKind::Count ); ++kind )
        summed += after.BytesPerOwnerKind[static_cast<std::size_t>( ResourceOwner::AssetService )][kind];
    EXPECT_EQ( after.BytesForOwner( ResourceOwner::AssetService ), summed );
}

TEST( ResourceCensusBytes, ACellWithRowsAndNoKnownSizeSaysSoRatherThanReadingAsEmpty )
{
    const ResourceCensus before = ResourceLedger::Take();

    // A GPU object whose owner never reported a footprint: legal, and the majority of rows on every
    // scene measured so far (566 of 602 on the world scene).
    auto unmeasured = ResourceOwnership::Take( ResourceKind::Framebuffer );
    unmeasured.Claim( ResourceOwner::SceneRenderer );

    const ResourceCensus after = ResourceLedger::Take();

    EXPECT_EQ( BytesDelta( before, after, ResourceOwner::SceneRenderer, ResourceKind::Framebuffer ), 0u );
    EXPECT_EQ( after.KnownFor( ResourceOwner::SceneRenderer, ResourceKind::Framebuffer ) -
                    before.KnownFor( ResourceOwner::SceneRenderer, ResourceKind::Framebuffer ),
               0u );
    EXPECT_EQ( after.PerOwnerKind[static_cast<std::size_t>( ResourceOwner::SceneRenderer )]
                                 [static_cast<std::size_t>( ResourceKind::Framebuffer )] -
                    before.PerOwnerKind[static_cast<std::size_t>( ResourceOwner::SceneRenderer )]
                                       [static_cast<std::size_t>( ResourceKind::Framebuffer )],
               1u )
         << "the row is not in the census at all, so 'no known size' cannot be distinguished from 'no row'";
}

TEST( ResourceCensusBytes, TheReportPrintsTheBytesAndTheirCoverageNextToEveryCount )
{
    auto texture = ResourceOwnership::Take( ResourceKind::Image2D, 5'592'405 );
    texture.Claim( ResourceOwner::AssetService );

    const std::string text = ResourceLedger::Report();

    // The line a person greps for when asking "how much of this scene is texture".
    EXPECT_NE( text.find( "AssetService" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "Image2D=" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "bytes=5592405" ), std::string::npos ) << text;
    // Coverage travels with the figure, for the same reason the grand total carries its own: a cell that
    // sums to zero because nobody measured it must not read like a cell that is empty.
    EXPECT_NE( text.find( " known=" ), std::string::npos ) << text;
}

TEST( ResourceCensusBytes, NoPerOwnerOrPerKindByteTotalIsStoredBesideTheTable )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // COMMENTS STRIPPED FIRST. A census that reads prose goes red on its own explanation of what it
    // forbids, and then it gets switched off and takes a real finding with it.
    const std::string header = Desert::Tests::ConsumerText::StripComments(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/ResourceLedger.hpp" ) );
    ASSERT_FALSE( header.empty() );

    // The table is the single statement of the quantity; a field holding a per-owner or per-kind byte
    // total is a second one, and the two disagree the day a writer is added to one of them.
    EXPECT_EQ( header.find( "BytesPerOwner[" ), std::string::npos )
         << "a per-owner byte total is stored beside the table it can be summed from";
    EXPECT_EQ( header.find( "BytesPerKind[" ), std::string::npos )
         << "a per-kind byte total is stored beside the table it can be summed from";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

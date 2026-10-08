// The render graph's pass ORDER, tested without a GPU.
//
// Until now the order of two passes inside one phase was whatever the containers handed back, and the
// only way to find out was to look at the frame: that is why MeshRenderer::UpdateCascades() had to be
// hoisted out of the graph entirely, and why "the far field under the particles" could not be promised.
// The rules
// are pure functions of integers now (Engine/Graphic/RenderGraphSort.hpp) and everything below asserts
// them directly.
//
// Nothing here touches Vulkan: RenderGraphBuilder itself creates RenderPass objects while it sorts, so
// the decision was deliberately moved out of it.

#include <Engine/Graphic/RenderGraphSort.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Graphic::OrderRenderPasses;
using Desert::Graphic::OrderRenderPhases;
using Desert::Graphic::RenderPassSortKey;
using Desert::Graphic::RenderPhaseDependencies;
using Desert::Graphic::RenderPhaseID;

namespace RenderPhase = Desert::Graphic::RenderPhase;
namespace fs          = std::filesystem;

namespace
{
    // The engine's real declaration order (RenderPhaseRegistry's constructor), which is what the
    // builder feeds the sort.
    std::vector<RenderPhaseID> BuiltinDeclarationOrder()
    {
        return { RenderPhase::k_BuiltinOrder, RenderPhase::k_BuiltinOrder + RenderPhase::k_BuiltinCount };
    }

    // The five phase edges SceneRenderer::RebuildRenderGraph declares.
    RenderPhaseDependencies EnginePhaseEdges()
    {
        RenderPhaseDependencies deps;
        deps[RenderPhase::Geometry]    = { RenderPhase::DepthPrePass, RenderPhase::Sky };
        deps[RenderPhase::Outline]     = { RenderPhase::Geometry };
        deps[RenderPhase::Lighting]    = { RenderPhase::Geometry };
        deps[RenderPhase::PostProcess] = { RenderPhase::Lighting };
        return deps;
    }

    // A pass as a test writes it down: a name plus the phase the sort reads (the registration index is its
    // position in the list).
    struct Pass
    {
        std::string   Name;
        RenderPhaseID Phase;
    };

    // Registers `passes` in the given order (index = order of the AddPass call) and returns the names
    // in execution order.
    std::vector<std::string> SortNames( const std::vector<Pass>&          passes,
                                        const std::vector<RenderPhaseID>& phaseOrder )
    {
        std::vector<RenderPassSortKey> keys;
        for ( std::size_t i = 0; i < passes.size(); ++i )
            keys.push_back( RenderPassSortKey{ passes[i].Phase, static_cast<uint64_t>( i ) } );

        std::vector<std::string> names;
        for ( std::size_t index : OrderRenderPasses( keys, phaseOrder ) )
            names.push_back( passes[index].Name );
        return names;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// Phase order: the topological part.
// ---------------------------------------------------------------------------------------------------

TEST( PhaseOrder, EngineEdgesProduceTheExpectedFrame )
{
    const std::set<RenderPhaseID> present = {
         RenderPhase::DepthPrePass, RenderPhase::Sky,      RenderPhase::Geometry,
         RenderPhase::Outline,      RenderPhase::Lighting, RenderPhase::Transparency,
         RenderPhase::PostProcess,  RenderPhase::UI,       RenderPhase::Debug };

    const std::vector<RenderPhaseID> order =
         OrderRenderPhases( present, EnginePhaseEdges(), BuiltinDeclarationOrder() );

    const std::vector<RenderPhaseID> expected = {
         RenderPhase::DepthPrePass, RenderPhase::Sky,      RenderPhase::Geometry,
         RenderPhase::Outline,      RenderPhase::Lighting, RenderPhase::Transparency,
         RenderPhase::PostProcess,  RenderPhase::UI,       RenderPhase::Debug };
    EXPECT_EQ( order, expected );
}

TEST( PhaseOrder, DependenciesBeatDeclarationOrder )
{
    // Debug is declared last but is made a prerequisite of Geometry: it must move to the front.
    RenderPhaseDependencies deps;
    deps[RenderPhase::Geometry] = { RenderPhase::Debug };

    const std::set<RenderPhaseID>    present = { RenderPhase::Geometry, RenderPhase::Debug };
    const std::vector<RenderPhaseID> order   = OrderRenderPhases( present, deps, BuiltinDeclarationOrder() );

    ASSERT_EQ( order.size(), 2u );
    EXPECT_EQ( order[0], RenderPhase::Debug );
    EXPECT_EQ( order[1], RenderPhase::Geometry );
}

TEST( PhaseOrder, APhaseNamedOnlyByAnEdgeStillConstrainsTheOrder )
{
    // Nobody registered a pass in Sky, but Geometry depends on it. Sky must still appear, ahead of
    // Geometry — otherwise an edge onto an empty phase would quietly mean nothing.
    const std::set<RenderPhaseID>    present = { RenderPhase::Geometry };
    const std::vector<RenderPhaseID> order =
         OrderRenderPhases( present, EnginePhaseEdges(), BuiltinDeclarationOrder() );

    const auto sky      = std::find( order.begin(), order.end(), RenderPhase::Sky );
    const auto geometry = std::find( order.begin(), order.end(), RenderPhase::Geometry );
    ASSERT_NE( sky, order.end() );
    ASSERT_NE( geometry, order.end() );
    EXPECT_LT( sky, geometry );
}

TEST( PhaseOrder, AnUnregisteredPhaseIsPlacedLastInsteadOfDropped )
{
    // A raw phase ID that was never registered used to lose its passes without a word, because the
    // sort seeded itself exclusively from the declaration order.
    const RenderPhaseID           stray   = RenderPhase::k_UserBase + 7;
    const std::set<RenderPhaseID> present = { RenderPhase::Geometry, stray };

    const std::vector<RenderPhaseID> order =
         OrderRenderPhases( present, EnginePhaseEdges(), BuiltinDeclarationOrder() );

    ASSERT_FALSE( order.empty() );
    EXPECT_EQ( order.back(), stray );
    EXPECT_NE( std::find( order.begin(), order.end(), RenderPhase::Geometry ), order.end() );
}

TEST( PhaseOrder, IsIdenticalWhenTheSameGraphIsBuiltTwice )
{
    const std::set<RenderPhaseID> present = {
         RenderPhase::DepthPrePass, RenderPhase::Sky,          RenderPhase::Geometry,    RenderPhase::Outline,
         RenderPhase::Lighting,     RenderPhase::Transparency, RenderPhase::PostProcess, RenderPhase::Debug };

    const std::vector<RenderPhaseID> first =
         OrderRenderPhases( present, EnginePhaseEdges(), BuiltinDeclarationOrder() );
    const std::vector<RenderPhaseID> second =
         OrderRenderPhases( present, EnginePhaseEdges(), BuiltinDeclarationOrder() );
    EXPECT_EQ( first, second );
}

// ---------------------------------------------------------------------------------------------------
// Pass order inside a phase — the part that was undefined.
// ---------------------------------------------------------------------------------------------------

TEST( PassOrder, PassesFollowTheirPhases )
{
    const std::vector<RenderPhaseID> phaseOrder =
         OrderRenderPhases( { RenderPhase::Sky, RenderPhase::Geometry, RenderPhase::Transparency },
                            EnginePhaseEdges(), BuiltinDeclarationOrder() );

    // Registered in a shuffled order, on purpose: the phase decides, not the call site.
    const std::vector<Pass> passes = {
         { "Particles", RenderPhase::Transparency },
         { "Mesh", RenderPhase::Geometry },
         { "Skybox", RenderPhase::Sky },
    };

    const std::vector<std::string> expected = { "Skybox", "Mesh", "Particles" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

TEST( PassOrder, EqualPassesKeepRegistrationOrder )
{
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Geometry };

    // What MeshRenderer and TerrainRenderer do today: the two passes Geometry actually holds, neither
    // of them explicitly placed. (There was a third, "GrassPass", until Г25 removed the procedural
    // grass generator; the tie-break argument is the same with two, and the 64-element case below is
    // what carries it past the size where a sort could keep the order by luck.)
    const std::vector<Pass> passes = {
         { "MeshGeometryPass", RenderPhase::Geometry },
         { "TerrainPass", RenderPhase::Geometry },
    };

    const std::vector<std::string> expected = { "MeshGeometryPass", "TerrainPass" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );

    // And it must still hold past the size where a sort stops being an insertion sort — with a handful
    // of elements even a comparator that ignores the tie-break keeps the input order by luck, which is
    // how "undefined" survives a review.
    std::vector<Pass>        many;
    std::vector<std::string> manyExpected;
    for ( int i = 0; i < 64; ++i )
    {
        const std::string name = "Pass" + std::to_string( i );
        many.push_back( Pass{ name, RenderPhase::Geometry } );
        manyExpected.push_back( name );
    }
    EXPECT_EQ( SortNames( many, phaseOrder ), manyExpected );
}

TEST( PassOrder, ShuffledRegistrationAcrossPhasesSortsByPhaseThenRegistration )
{
    const std::vector<RenderPhaseID> phaseOrder =
         OrderRenderPhases( { RenderPhase::DepthPrePass, RenderPhase::Sky, RenderPhase::Geometry,
                              RenderPhase::Transparency, RenderPhase::Debug },
                            EnginePhaseEdges(), BuiltinDeclarationOrder() );

    const std::vector<Pass> passes = {
         { "DebugLines", RenderPhase::Debug },
         { "Terrain", RenderPhase::Geometry },
         { "Cascade0", RenderPhase::DepthPrePass },
         { "Particles", RenderPhase::Transparency },
         { "Mesh", RenderPhase::Geometry },
         { "Cascade1", RenderPhase::DepthPrePass },
         { "Skybox", RenderPhase::Sky },
    };

    const std::vector<std::string> expected = { "Cascade0", "Cascade1",  "Skybox",    "Terrain",
                                                "Mesh",     "Particles", "DebugLines" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

TEST( PassOrder, IsIdenticalWhenTheSameGraphIsBuiltTwice )
{
    const std::vector<RenderPhaseID> phaseOrder =
         OrderRenderPhases( { RenderPhase::Geometry, RenderPhase::Transparency, RenderPhase::UI },
                            EnginePhaseEdges(), BuiltinDeclarationOrder() );

    const std::vector<Pass> passes = {
         { "Canvas", RenderPhase::UI },        { "Backdrop", RenderPhase::Transparency },
         { "Mesh", RenderPhase::Geometry },    { "Particles", RenderPhase::Transparency },
         { "Terrain", RenderPhase::Geometry },
    };

    EXPECT_EQ( SortNames( passes, phaseOrder ), SortNames( passes, phaseOrder ) );
}

TEST( PassOrder, ADuplicateRegistrationIndexStillGivesOneDefinedOrder )
{
    // The registration index is unique by construction, but the sort must not degrade into
    // "unspecified" if a caller ever hands in equal keys — that is the very failure being fixed.
    //
    // Deliberately more passes than a sort switches to insertion sort for: std::sort leaves equal
    // elements wherever the partitioning put them, and with a handful of elements that accidentally
    // looks like input order. This is exactly how an undefined order passes review.
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Geometry };

    std::vector<RenderPassSortKey> keys;
    std::vector<std::size_t>       expected;
    for ( std::size_t i = 0; i < 64; ++i )
    {
        keys.push_back( RenderPassSortKey{ RenderPhase::Geometry, 4 } );
        expected.push_back( i );
    }

    EXPECT_EQ( OrderRenderPasses( keys, phaseOrder ), expected );
    EXPECT_EQ( OrderRenderPasses( keys, phaseOrder ), OrderRenderPasses( keys, phaseOrder ) );
}

TEST( PassOrder, APassInAnUnorderedPhaseIsDrawnLastRatherThanLost )
{
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Geometry };

    const std::vector<Pass> passes = {
         { "Stray", RenderPhase::k_UserBase },
         { "Mesh", RenderPhase::Geometry },
    };

    const std::vector<std::string> expected = { "Mesh", "Stray" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

// ARCH1b: there is no numeric placement of a pass. An order a pass depends on (the fog under the far field
// under the particles) is the order of the calls in SceneRenderer's frame-build functions, asserted by
// RenderGraphCompile.SceneRendererAddsItsPassesInTheFrameOrder. Red when any engine or editor source names the
// removed ladder or a per-pass order field again.
TEST( PassOrder, EngineAndEditorHaveNoNumericPassPlacement )
{
    fs::path root = ".";
    for ( int up = 0; up < 6 && !fs::exists( root / "Desert/Desert/Source/Engine" ); ++up )
        root /= "..";
    ASSERT_TRUE( fs::exists( root / "Desert/Desert/Source/Engine" ) ) << "run from inside the repository";

    std::vector<std::string> offenders;
    std::size_t              scanned = 0;
    for ( const char* tree : { "Desert/Desert/Source/Engine", "Editor/Source" } )
    {
        ASSERT_TRUE( fs::exists( root / tree ) ) << tree << " is gone";
        for ( const auto& entry : fs::recursive_directory_iterator( root / tree ) )
        {
            const std::string extension = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( extension != ".hpp" && extension != ".cpp" && extension != ".h" ) )
                continue;
            std::ifstream     file( entry.path(), std::ios::binary );
            std::stringstream text;
            text << file.rdbuf();
            ++scanned;
            const std::string source = text.str();
            for ( const char* symbol : { "RenderPassOrder", "OrderInPhase" } )
            {
                if ( source.find( symbol ) != std::string::npos )
                    offenders.push_back( entry.path().generic_string() + ": " + symbol );
            }
        }
    }
    EXPECT_GT( scanned, 100u ) << "the census read almost nothing; the roots moved";
    EXPECT_TRUE( offenders.empty() ) << ::testing::PrintToString( offenders );
}

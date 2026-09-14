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

#include <string>
#include <vector>

using Desert::Graphic::OrderRenderPasses;
using Desert::Graphic::OrderRenderPhases;
using Desert::Graphic::RenderPassOrderKey;
using Desert::Graphic::RenderPhaseDependencies;
using Desert::Graphic::RenderPhaseID;

namespace RenderPhase     = Desert::Graphic::RenderPhase;
namespace RenderPassOrder = Desert::Graphic::RenderPassOrder;

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

    // A pass as a test writes it down: a name plus the three numbers the sort actually reads.
    struct Pass
    {
        std::string   Name;
        RenderPhaseID Phase;
        int32_t       OrderInPhase;
    };

    // Registers `passes` in the given order (index = order of the AddPass call) and returns the names
    // in execution order.
    std::vector<std::string> SortNames( const std::vector<Pass>&          passes,
                                        const std::vector<RenderPhaseID>& phaseOrder )
    {
        std::vector<RenderPassOrderKey> keys;
        for ( std::size_t i = 0; i < passes.size(); ++i )
        {
            keys.push_back(
                 RenderPassOrderKey{ passes[i].Phase, passes[i].OrderInPhase, static_cast<uint64_t>( i ) } );
        }

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
         { "Particles", RenderPhase::Transparency, RenderPassOrder::Default },
         { "Mesh", RenderPhase::Geometry, RenderPassOrder::Default },
         { "Skybox", RenderPhase::Sky, RenderPassOrder::Default },
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
         { "MeshGeometryPass", RenderPhase::Geometry, RenderPassOrder::Default },
         { "TerrainPass", RenderPhase::Geometry, RenderPassOrder::Default },
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
        many.push_back( Pass{ name, RenderPhase::Geometry, RenderPassOrder::Default } );
        manyExpected.push_back( name );
    }
    EXPECT_EQ( SortNames( many, phaseOrder ), manyExpected );
}

TEST( PassOrder, ExplicitPlacementOverrulesRegistrationOrder )
{
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Transparency };

    // The pass registered LAST asks to be first, and gets it. This is the escape hatch a pass uses
    // when its position is a visual requirement rather than an accident of the Init call order.
    const std::vector<Pass> passes = {
         { "Particles", RenderPhase::Transparency, RenderPassOrder::Default },
         { "Backdrop", RenderPhase::Transparency, RenderPassOrder::FarField },
    };

    const std::vector<std::string> expected = { "Backdrop", "Particles" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

TEST( PassOrder, NearFieldSortsAfterDefaultAndFarField )
{
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Transparency };

    const std::vector<Pass> passes = {
         { "Near", RenderPhase::Transparency, RenderPassOrder::NearField },
         { "Far", RenderPhase::Transparency, RenderPassOrder::FarField },
         { "Middle", RenderPhase::Transparency, RenderPassOrder::Default },
    };

    const std::vector<std::string> expected = { "Far", "Middle", "Near" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

// The case this task exists to make possible: a far-field composite and the particle billboards share
// RenderPhase::Transparency. Sparks and smoke from an emitter in front of the camera must sit OVER the
// distant backdrop — the other way round is the same class of mistake as the particle "top-down" bug.
TEST( PassOrder, FarFieldCompositesBeforeParticlesInTransparency )
{
    const std::vector<RenderPhaseID> phaseOrder = OrderRenderPhases(
         { RenderPhase::Geometry, RenderPhase::Transparency }, EnginePhaseEdges(), BuiltinDeclarationOrder() );

    // Registration order agrees with the intent (the backdrop first) ...
    const std::vector<Pass> registeredBackdropFirst = {
         { "BackdropCompositePass", RenderPhase::Transparency, RenderPassOrder::FarField },
         { "ParticlePass", RenderPhase::Transparency, RenderPassOrder::Default },
    };

    // ... and here it contradicts it. The backdrop must still come first: the requirement is a property
    // of the passes, not of the order SceneRenderer::Init happens to register their systems in.
    const std::vector<Pass> registeredParticlesFirst = {
         { "ParticlePass", RenderPhase::Transparency, RenderPassOrder::Default },
         { "BackdropCompositePass", RenderPhase::Transparency, RenderPassOrder::FarField },
    };

    const std::vector<std::string> expected = { "BackdropCompositePass", "ParticlePass" };
    EXPECT_EQ( SortNames( registeredBackdropFirst, phaseOrder ), expected );
    EXPECT_EQ( SortNames( registeredParticlesFirst, phaseOrder ), expected );
}

// The height-fog apply is the FLOOR of the Transparency phase: it modifies the opaque scene itself, so
// everything the phase composites over that scene — the far field and every particle — must land on top
// of it. Two constants that must agree (AtmosphericFog below FarField below Default); each is
// individually plausible, which is exactly why the agreement is asserted rather than assumed.
TEST( PassOrder, HeightFogAppliesUnderTheFarFieldAndTheParticles )
{
    const std::vector<RenderPhaseID> phaseOrder = OrderRenderPhases(
         { RenderPhase::Geometry, RenderPhase::Transparency }, EnginePhaseEdges(), BuiltinDeclarationOrder() );

    // Registered in the worst possible order — the fog last, after everything that must draw over it.
    const std::vector<Pass> passes = {
         { "ParticlePass", RenderPhase::Transparency, RenderPassOrder::Default },
         { "BackdropCompositePass", RenderPhase::Transparency, RenderPassOrder::FarField },
         { "HeightFogApply", RenderPhase::Transparency, RenderPassOrder::AtmosphericFog },
    };

    const std::vector<std::string> expected = { "HeightFogApply", "BackdropCompositePass", "ParticlePass" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );

    // Said as the relation itself, so a future pass inserted between them cannot quietly reorder these.
    EXPECT_LT( RenderPassOrder::AtmosphericFog, RenderPassOrder::FarField );
    EXPECT_LT( RenderPassOrder::FarField, RenderPassOrder::Default );
}

TEST( PassOrder, ShuffledRegistrationAcrossPhasesSortsByPhaseThenRegistration )
{
    const std::vector<RenderPhaseID> phaseOrder =
         OrderRenderPhases( { RenderPhase::DepthPrePass, RenderPhase::Sky, RenderPhase::Geometry,
                              RenderPhase::Transparency, RenderPhase::Debug },
                            EnginePhaseEdges(), BuiltinDeclarationOrder() );

    const std::vector<Pass> passes = {
         { "DebugLines", RenderPhase::Debug, RenderPassOrder::Default },
         { "Terrain", RenderPhase::Geometry, RenderPassOrder::Default },
         { "Cascade0", RenderPhase::DepthPrePass, RenderPassOrder::Default },
         { "Particles", RenderPhase::Transparency, RenderPassOrder::Default },
         { "Mesh", RenderPhase::Geometry, RenderPassOrder::Default },
         { "Cascade1", RenderPhase::DepthPrePass, RenderPassOrder::Default },
         { "Skybox", RenderPhase::Sky, RenderPassOrder::Default },
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
         { "Canvas", RenderPhase::UI, RenderPassOrder::Default },
         { "Backdrop", RenderPhase::Transparency, RenderPassOrder::FarField },
         { "Mesh", RenderPhase::Geometry, RenderPassOrder::Default },
         { "Particles", RenderPhase::Transparency, RenderPassOrder::Default },
         { "Terrain", RenderPhase::Geometry, RenderPassOrder::Default },
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

    std::vector<RenderPassOrderKey> keys;
    std::vector<std::size_t>        expected;
    for ( std::size_t i = 0; i < 64; ++i )
    {
        keys.push_back( RenderPassOrderKey{ RenderPhase::Geometry, RenderPassOrder::Default, 4 } );
        expected.push_back( i );
    }

    EXPECT_EQ( OrderRenderPasses( keys, phaseOrder ), expected );
    EXPECT_EQ( OrderRenderPasses( keys, phaseOrder ), OrderRenderPasses( keys, phaseOrder ) );
}

TEST( PassOrder, APassInAnUnorderedPhaseIsDrawnLastRatherThanLost )
{
    const std::vector<RenderPhaseID> phaseOrder = { RenderPhase::Geometry };

    const std::vector<Pass> passes = {
         { "Stray", RenderPhase::k_UserBase, RenderPassOrder::FarField },
         { "Mesh", RenderPhase::Geometry, RenderPassOrder::Default },
    };

    const std::vector<std::string> expected = { "Mesh", "Stray" };
    EXPECT_EQ( SortNames( passes, phaseOrder ), expected );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

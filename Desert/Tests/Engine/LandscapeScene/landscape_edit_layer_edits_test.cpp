// WHAT ONE EDIT LAYERS ACTION DOES TO A LANDSCAPE, AND WHAT ITS UNDO PUTS BACK.
//
// The unit is Editor/Core/Commands/LandscapeEditLayerEdits.cpp, the scene-free half of the panel's commands
// (LandscapeLayerCommands.cpp): the command's redo applies the edited stack, its undo applies the stack it had
// before plus the removed layer's tile data, both through ApplyLandscapeEditLayerStack. So each case below is
// "apply after, look at the merged samples; apply before, look again".

#include <Editor/Core/Commands/LandscapeEditLayerEdits.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

using namespace Desert;
using namespace Desert::World::Landscape;
using namespace Desert::Editor::Commands;

namespace
{
    constexpr uint32_t kSamples = 8u;
    constexpr uint16_t kGround  = 30000u;
    constexpr uint16_t kRaise   = 500u;

    struct Fixture
    {
        entt::registry registry;
        Common::UUID   landscape{ 7001u };
        Common::UUID   base{ kLandscapeBaseEditLayerGuid };
        Common::UUID   upper{ 7002u };
        entt::entity   tile = entt::null;

        Fixture()
        {
            const auto root                                   = registry.create();
            registry.emplace<ECS::UUIDComponent>( root ).UUID = landscape;
            auto& body                                        = registry.emplace<ECS::LandscapeComponent>( root );
            body.QuadsPerTile                                 = kSamples - 1u;
            body.EditLayers.Layers.push_back( { base, "Base" } );

            auto made = LandscapeTileData::FromSamples(
                 kSamples, kSamples,
                 std::vector<uint16_t>( static_cast<size_t>( kSamples ) * kSamples, kGround ) );
            EXPECT_TRUE( made.IsSuccess() );
            LandscapeTileData data = made.ExtractValue();
            EXPECT_TRUE( data.SetEditLayer( { base, data.Samples(), {} } ).IsSuccess() );
            tile                = registry.create();
            auto& component     = registry.emplace<ECS::LandscapeTileComponent>( tile );
            component.Landscape = landscape;
            component.Heights   = std::move( data );
        }

        LandscapeEditLayerStack& Stack()
        {
            return registry.get<ECS::LandscapeComponent>( ECS::FindLandscapeRootEntity( registry, landscape ) )
                 .EditLayers;
        }
        LandscapeTileData& Tile()
        {
            auto& heights = registry.get<ECS::LandscapeTileComponent>( tile ).Heights;
            if ( !heights )
                throw std::logic_error( "fixture tile has no heights" );
            return *heights;
        }
        uint16_t Height()
        {
            return Tile().Samples()[3 * kSamples + 4];
        }
        Common::BoolResultStr Apply( const LandscapeEditLayerStack&             stack,
                                     std::span<const LandscapeRemovedTileLayer> restore = {} )
        {
            return ApplyLandscapeEditLayerStack( registry, landscape, stack, {}, restore, true );
        }

        // "Create Layer" then a stroke of +kRaise everywhere into it, merged: what the live frame does.
        LandscapeEditLayerStack AddAndStroke()
        {
            const auto added = LandscapeStackWithLayerAdded( Stack(), Common::UUID::Null(), upper );
            EXPECT_TRUE( ApplyLandscapeEditLayerStack( registry, landscape, added, {}, {}, false ).IsSuccess() );
            EXPECT_TRUE( Tile()
                              .SetEditLayer( { upper,
                                               std::vector<uint16_t>(
                                                    static_cast<size_t>( kSamples ) * kSamples,
                                                    static_cast<uint16_t>( kLandscapeMidSample + kRaise ) ),
                                               {} } )
                              .IsSuccess() );
            EXPECT_TRUE( Apply( added ).IsSuccess() );
            return added;
        }
    };
} // namespace

TEST( LandscapeEditLayerEdits, ANewLayerGoesAboveTheEditingLayerWithTheFirstFreeName )
{
    Fixture    f;
    const auto added = LandscapeStackWithLayerAdded( f.Stack(), f.base, f.upper );
    ASSERT_EQ( added.Layers.size(), 2u );
    EXPECT_EQ( static_cast<uint64_t>( added.Layers[1].Guid ), static_cast<uint64_t>( f.upper ) );
    EXPECT_EQ( added.Layers[1].Name, "Layer 2" );
    const auto third = LandscapeStackWithLayerAdded( added, f.base, Common::UUID( 7003u ) );
    ASSERT_EQ( third.Layers.size(), 3u );
    EXPECT_EQ( static_cast<uint64_t>( third.Layers[1].Guid ), 7003u ) << "above the editing layer, not on top";
    EXPECT_EQ( third.Layers[1].Name, "Layer 3" );
}

TEST( LandscapeEditLayerEdits, HidingTheStrokedLayerGivesTheGroundBackAndShowingItRaisesItAgain )
{
    Fixture    f;
    const auto shown = f.AddAndStroke();
    ASSERT_EQ( f.Height(), kGround + kRaise );
    auto hidden              = shown;
    hidden.Layers[1].Visible = false;
    ASSERT_TRUE( f.Apply( hidden ).IsSuccess() );
    EXPECT_EQ( f.Height(), kGround );
    ASSERT_TRUE( f.Apply( shown ).IsSuccess() ); // the hide's undo
    EXPECT_EQ( f.Height(), kGround + kRaise );
}

TEST( LandscapeEditLayerEdits, AHalfHeightAlphaHalvesTheLayersContribution )
{
    Fixture    f;
    const auto full            = f.AddAndStroke();
    auto       half            = full;
    half.Layers[1].HeightAlpha = 0.5f;
    ASSERT_TRUE( f.Apply( half ).IsSuccess() );
    EXPECT_EQ( f.Height(), kGround + kRaise / 2u );
    auto outside                  = full;
    outside.Layers[1].HeightAlpha = 1.5f;
    const auto refused            = f.Apply( outside );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "1.5" ), std::string::npos ) << refused.GetError();
    EXPECT_EQ( f.Height(), kGround + kRaise / 2u ) << "a refused stack writes nothing";
}

TEST( LandscapeEditLayerEdits, DeletingALayerDropsItsDataAndUndoPutsItBack )
{
    Fixture    f;
    const auto before  = f.AddAndStroke();
    auto       removed = LandscapeEditLayerTileDataOf( f.registry, f.landscape, f.upper );
    ASSERT_TRUE( removed.IsSuccess() ) << removed.GetError();
    ASSERT_EQ( removed.GetValue().size(), 1u );
    auto after = LandscapeStackWithLayerRemoved( before, f.upper );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    ASSERT_TRUE( f.Apply( after.GetValue() ).IsSuccess() );
    EXPECT_EQ( f.Height(), kGround );
    EXPECT_EQ( f.Tile().FindEditLayer( f.upper ), nullptr );

    ASSERT_TRUE( f.Apply( before, removed.GetValue() ).IsSuccess() ); // the delete's undo
    EXPECT_EQ( f.Height(), kGround + kRaise );
    EXPECT_NE( f.Tile().FindEditLayer( f.upper ), nullptr );
}

TEST( LandscapeEditLayerEdits, TheLastLayerIsNotDeleted )
{
    Fixture    f;
    const auto refused = LandscapeStackWithLayerRemoved( f.Stack(), f.base );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "last layer" ), std::string::npos ) << refused.GetError();
}

TEST( LandscapeEditLayerEdits, AMergingEditRefusesAnUnloadedTileByNameAndWritesNothing )
{
    Fixture    f;
    const auto shown         = f.AddAndStroke();
    const auto other         = f.registry.create();
    auto&      tile          = f.registry.emplace<ECS::LandscapeTileComponent>( other );
    tile.Landscape           = f.landscape;
    tile.TileX               = 1;
    auto hidden              = shown;
    hidden.Layers[1].Visible = false;
    const auto refused       = f.Apply( hidden );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "tile 1_0" ), std::string::npos ) << refused.GetError();
    EXPECT_TRUE( f.Stack().Layers[1].Visible );
    EXPECT_EQ( f.Height(), kGround + kRaise );
}

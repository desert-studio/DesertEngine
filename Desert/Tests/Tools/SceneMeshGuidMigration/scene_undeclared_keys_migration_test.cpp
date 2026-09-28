// MigrateUndeclaredKeysV38ToV39 (SAVE1): the four hand-authoring slips the lenient reader used to skip are
// settled in the file - UIToggle.On and UIButton.CornerRadius go, a UIButton.Action NAME becomes SendEvent +
// that name as the Action Target, a light's Falloff NAME becomes its number - and what cannot be settled
// without guessing is refused by name.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Components.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    rfl::Generic::Object BlockOf( const rfl::ExtraFields<rfl::Generic>& components, const char* component )
    {
        return components.get( component ).value().to_object().value();
    }

    EntityData WithBlock( const char* component, rfl::Generic::Object block )
    {
        EntityData entity;
        entity.Tag                   = "Probe";
        entity.Components[component] = rfl::Generic( std::move( block ) );
        return entity;
    }
} // namespace

TEST( SceneUndeclaredKeysMigration, VersionIsTheHeadGeneration )
{
    EXPECT_EQ( Migration::kSceneVersionNoUndeclaredKeys, 39 );
    EXPECT_EQ( Desert::Core::kSceneVersion, Migration::kSceneVersionNoUndeclaredKeys );
}

TEST( SceneUndeclaredKeysMigration, ToggleOnGoesAndValueStays )
{
    rfl::Generic::Object toggle;
    toggle["On"]    = rfl::Generic( true );
    toggle["Value"] = rfl::Generic( false );
    std::vector<EntityData> entities{ WithBlock( "UIToggle", toggle ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    EXPECT_EQ( report.TogglesOnDropped, 1u );
    EXPECT_TRUE( report.Refused.empty() );
    const auto block = BlockOf( entities[0].Components, "UIToggle" );
    EXPECT_FALSE( block.get( "On" ).has_value() );
    // The build read Value, never On: the toggle keeps the state it has always been drawn with.
    EXPECT_FALSE( block.get( "Value" ).value().to_bool().value() );
}

TEST( SceneUndeclaredKeysMigration, ButtonCornerRadiusGoesAndTheColoursStay )
{
    rfl::Generic::Object button;
    button["CornerRadius"] = rfl::Generic( 12.0 );
    button["Action"]       = rfl::Generic( 6 );
    std::vector<EntityData> entities{ WithBlock( "UIButton", button ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    EXPECT_EQ( report.ButtonCornerRadiiDropped, 1u );
    EXPECT_EQ( report.ButtonActionNamesMoved, 0u );
    const auto block = BlockOf( entities[0].Components, "UIButton" );
    EXPECT_FALSE( block.get( "CornerRadius" ).has_value() );
    EXPECT_EQ( block.get( "Action" ).value().to_int().value(), 6 );
}

TEST( SceneUndeclaredKeysMigration, ActionNameBecomesSendEventWithThatTarget )
{
    rfl::Generic::Object button;
    button["Action"] = rfl::Generic( std::string( "ui.over.probe" ) );
    std::vector<EntityData> entities{ WithBlock( "UIButton", button ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    EXPECT_EQ( report.ButtonActionNamesMoved, 1u );
    const auto block = BlockOf( entities[0].Components, "UIButton" );
    EXPECT_EQ( block.get( "Action" ).value().to_int().value(),
               static_cast<int>( Desert::ECS::UIButtonAction::SendEvent ) );
    EXPECT_EQ( block.get( "OnClickMessage" ).value().to_string().value(), "ui.over.probe" );
}

TEST( SceneUndeclaredKeysMigration, ActionNameAgainstAnotherTargetIsRefusedByName )
{
    rfl::Generic::Object button;
    button["Action"]         = rfl::Generic( std::string( "ui.over.probe" ) );
    button["OnClickMessage"] = rfl::Generic( std::string( "Scenes/Other.desce" ) );
    std::vector<EntityData> entities{ WithBlock( "UIButton", button ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    ASSERT_EQ( report.Refused.size(), 1u );
    EXPECT_NE( report.Refused[0].find( "'Probe'" ), std::string::npos ) << report.Refused[0];
    EXPECT_NE( report.Refused[0].find( "Scenes/Other.desce" ), std::string::npos ) << report.Refused[0];
    EXPECT_EQ( BlockOf( entities[0].Components, "UIButton" ).get( "Action" ).value().to_string().value(),
               "ui.over.probe" );
}

TEST( SceneUndeclaredKeysMigration, FalloffNameBecomesItsNumberOnRecordsAndOverrides )
{
    rfl::Generic::Object light;
    light["Falloff"] = rfl::Generic( std::string( "Quadratic" ) );
    std::vector<EntityData> entities{ WithBlock( "PointLight", light ) };
    PrefabOverrideData      override_;
    rfl::Generic::Object    spot;
    spot["Falloff"]                   = rfl::Generic( std::string( "InverseSquare" ) );
    override_.Components["SpotLight"] = rfl::Generic( std::move( spot ) );
    entities[0].PrefabOverrides       = std::vector<PrefabOverrideData>{ override_ };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    EXPECT_EQ( report.FalloffNamesNumbered, 2u );
    EXPECT_EQ( BlockOf( entities[0].Components, "PointLight" ).get( "Falloff" ).value().to_int().value(),
               static_cast<int>( Desert::ECS::LightFalloff::Quadratic ) );
    EXPECT_EQ( BlockOf( entities[0].PrefabOverrides->at( 0 ).Components, "SpotLight" )
                    .get( "Falloff" )
                    .value()
                    .to_int()
                    .value(),
               static_cast<int>( Desert::ECS::LightFalloff::InverseSquare ) );
}

TEST( SceneUndeclaredKeysMigration, UnknownFalloffNameIsRefused )
{
    rfl::Generic::Object light;
    light["Falloff"] = rfl::Generic( std::string( "Cubic" ) );
    std::vector<EntityData> entities{ WithBlock( "PointLight", light ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    ASSERT_EQ( report.Refused.size(), 1u );
    EXPECT_NE( report.Refused[0].find( "Cubic" ), std::string::npos ) << report.Refused[0];
    EXPECT_EQ( report.FalloffNamesNumbered, 0u );
}

TEST( SceneUndeclaredKeysMigration, DeclaredBlocksAreUntouched )
{
    rfl::Generic::Object light;
    light["Falloff"] = rfl::Generic( 1 );
    rfl::Generic::Object toggle;
    toggle["Value"] = rfl::Generic( true );
    std::vector<EntityData> entities{ WithBlock( "PointLight", light ), WithBlock( "UIToggle", toggle ) };

    const auto report = Migration::MigrateUndeclaredKeysV38ToV39( entities );

    EXPECT_EQ( report.Rewritten(), 0u );
    EXPECT_TRUE( report.Refused.empty() );
    EXPECT_EQ( BlockOf( entities[0].Components, "PointLight" ).get( "Falloff" ).value().to_int().value(), 1 );
}
// NOLINTEND(bugprone-unchecked-optional-access)

// SET1: SceneSettings' grade and shadow fields left the level block for their UE homes — a PostProcessVolume
// entity (blended per view by Graphic::ResolveViewSettings, UE's FFinalPostProcessSettings) and the
// DirectionalLight. Two halves are proven here and, more importantly, their RELATION: a scene raised by the
// v36 step resolves, through the same function the renderer calls, to exactly the values its Settings block
// stated — which is why the corpus frames before and after the migration can be byte-equal.

#include <SceneMigration.hpp>

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace Desert;

namespace
{
    // A v32 scene: a sun, a mesh-less child, and a Settings block stating grade, shadow and level keys.
    const char* kSceneV32 = R"({
        "Header": { "Kind": "Scene", "Guid": "2da2387a87064ed6bd33c61d6d5a7fc7",
                    "Versions": { "SCNE": 32, "UNIT": 1 }, "Dependencies": [] },
        "SceneName": "SET1 probe",
        "Entities": [
            { "id": 11, "siblingIndex": 0, "Tag": "Sun", "Translation": [-35.0, -90.0, -25.0],
              "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0],
              "DirectionLight": { "Color": [1.0, 0.9, 0.8], "Intensity": 2.0 } },
            { "id": 12, "siblingIndex": 3, "Tag": "Other", "Translation": [0.0, 0.0, 0.0],
              "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0] }
        ],
        "Settings": {
            "RenderingPath": 0, "Gravity": 500.0,
            "EnableShadows": false, "ShadowBias": 0.0125, "CascadeSplitLambda": 0.85,
            "Tonemapper": 1, "Exposure": 2.5, "Gamma": 2.4, "WhitePoint": 11.0,
            "AutoExposure": true, "AutoExposureKey": 0.21,
            "EnableBloom": true, "BloomIntensity": 1.3, "LensFlareTint": [0.5, 0.25, 1.0],
            "LensFlareGhostCount": 6, "GlobalIllumination": 2, "EnableSSR": true, "SSRMaxDistance": 2500.0
        }
    })";

    Migration::SceneSerialized Parse( const std::string& json )
    {
        auto parsed = rfl::json::read<Migration::SceneSerialized>( json );
        EXPECT_TRUE( parsed.has_value() ) << ( parsed.has_value() ? "" : parsed.error().what() );
        return parsed.has_value() ? parsed.value() : Migration::SceneSerialized{};
    }

    const Assets::EntityData* FindTag( const Migration::SceneSerialized& scene, const std::string& tag )
    {
        for ( const auto& e : scene.Entities )
            if ( e.Tag == tag )
                return &e;
        return nullptr;
    }

    template <typename T>
    T Read( const Assets::EntityData& entity, const char* component, const char* typeName )
    {
        T                    result{};
        const auto           payload = entity.Components.get( component );
        const auto*          type    = Reflection::ReflectionRegistry::Get().Find( typeName );
        Common::Json::Issues issues;
        EXPECT_TRUE( payload.has_value() ) << component;
        EXPECT_NE( type, nullptr ) << typeName;
        if ( payload.has_value() && type )
            Reflection::DeserializeReflected( *type, &result, Common::Json::Root( payload.value() ), issues );
        EXPECT_TRUE( issues.empty() ) << component << ": " << issues.size() << " issue(s)";
        return result;
    }

    // The registry the renderer would see for the migrated scene: the volume and the light, nothing else.
    entt::registry RegistryOf( const Migration::SceneSerialized& scene )
    {
        entt::registry reg;
        if ( const auto* v = FindTag( scene, "PostProcessVolume" ) )
        {
            const entt::entity e = reg.create();
            reg.emplace<ECS::TransformComponent>( e );
            reg.emplace<ECS::PostProcessVolumeComponent>(
                 e, ECS::PostProcessVolumeComponent{
                         Read<ECS::PostProcessVolumeData>( *v, "PostProcessVolume", "PostProcessVolumeData" ) } );
        }
        if ( const auto* sun = FindTag( scene, "Sun" ) )
        {
            const entt::entity e                                  = reg.create();
            reg.emplace<ECS::TransformComponent>( e ).Translation = *sun->Translation;
            reg.emplace<ECS::DirectionLightComponent>(
                 e, ECS::DirectionLightComponent{
                         Read<ECS::DirectionalLightData>( *sun, "DirectionLight", "DirectionalLightData" ) } );
        }
        return reg;
    }

    entt::entity AddVolume( entt::registry& reg, const glm::vec3& at, const ECS::PostProcessVolumeData& data )
    {
        const entt::entity e                                  = reg.create();
        reg.emplace<ECS::TransformComponent>( e ).Translation = at;
        reg.emplace<ECS::PostProcessVolumeComponent>( e, ECS::PostProcessVolumeComponent{ data } );
        return e;
    }

    ECS::PostProcessVolumeData Volume( bool unbound, float priority, float weight, float exposure, bool bloom )
    {
        ECS::PostProcessVolumeData d;
        d.Unbound              = unbound;
        d.Priority             = priority;
        d.BlendWeight          = weight;
        d.Settings.Exposure    = exposure;
        d.Settings.EnableBloom = bloom;
        return d;
    }
} // namespace

// ── The migration ────────────────────────────────────────────────────────────────────────────────────────

TEST( SceneSettingsHomesMigration, GradeKeysLeaveForAnUnboundVolumeAndShadowKeysForTheLight )
{
    auto       scene  = Parse( kSceneV32 );
    const auto report = Migration::MigrateScene( scene, "", "" );
    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.SceneSettingsHomesRaised );
    EXPECT_TRUE( report.SceneSettingsHomes.VolumeCreated );
    EXPECT_EQ( report.SceneSettingsHomes.PostKeysMoved, 13 );
    EXPECT_EQ( report.SceneSettingsHomes.ShadowKeysFound, 3 );
    EXPECT_EQ( report.SceneSettingsHomes.LightsStamped, 1 );

    // What stays is what SceneSettings still declares — and the loader reads nothing else.
    const auto settings = scene.Settings.value().to_object().value();
    EXPECT_EQ( settings.size(), 2u );
    EXPECT_TRUE( settings.get( "RenderingPath" ).has_value() );
    EXPECT_TRUE( settings.get( "Gravity" ).has_value() );

    const auto* volume = FindTag( scene, "PostProcessVolume" );
    ASSERT_NE( volume, nullptr );
    EXPECT_FALSE( volume->parent.has_value() );
    EXPECT_EQ( volume->siblingIndex.value_or( 0 ), 4u ) << "appended after the last root sibling";
    const auto data = Read<ECS::PostProcessVolumeData>( *volume, "PostProcessVolume", "PostProcessVolumeData" );
    EXPECT_TRUE( data.Unbound );
    EXPECT_EQ( data.BlendWeight, 1.0f );
    EXPECT_EQ( data.Settings.Exposure, 2.5f );
    EXPECT_EQ( data.Settings.Tonemapper, Core::TonemapOperator::Reinhard );
    EXPECT_EQ( data.Settings.LensFlareGhostCount, 6 );

    const auto light =
         Read<ECS::DirectionalLightData>( *FindTag( scene, "Sun" ), "DirectionLight", "DirectionalLightData" );
    EXPECT_FALSE( light.CastShadows );
    EXPECT_EQ( light.ShadowBias, 0.0125f );
    EXPECT_EQ( light.CascadeSplitLambda, 0.85f );
    EXPECT_EQ( light.Intensity, 2.0f ) << "the light's own keys survive the stamp";

    EXPECT_TRUE( Core::SceneIsAtCurrentVersion( Core::SceneSerialized{
         scene.Header, scene.SceneName, scene.Entities, std::nullopt, scene.WorldPartition } ) );
}

TEST( SceneSettingsHomesMigration, ASecondRunMintsNoSecondVolumeAndTheIdIsStable )
{
    auto a = Parse( kSceneV32 );
    auto b = Parse( kSceneV32 );
    ASSERT_TRUE( Migration::MigrateScene( a, "", "" ).Refused.empty() );
    ASSERT_TRUE( Migration::MigrateScene( b, "", "" ).Refused.empty() );
    EXPECT_EQ( FindTag( a, "PostProcessVolume" )->id, FindTag( b, "PostProcessVolume" )->id )
         << "two branches migrating one scene must mint one entity";

    const std::size_t count  = a.Entities.size();
    const auto        second = Migration::MigrateScene( a, "", "" );
    EXPECT_FALSE( second.SceneSettingsHomesRaised );
    EXPECT_EQ( a.Entities.size(), count );
}

TEST( SceneSettingsHomesMigration, ABlockStatingNoGradeKeyGetsNoVolume )
{
    auto       scene  = Parse( R"({ "Header": { "Kind": "Scene", "Guid": "00000000000000000000000000000001",
        "Versions": { "SCNE": 32, "UNIT": 1 }, "Dependencies": [] }, "SceneName": "bare", "Entities": [],
        "Settings": { "Gravity": 981.0, "EnableShadows": true } })" );
    const auto report = Migration::MigrateScene( scene, "", "" );
    ASSERT_TRUE( report.Refused.empty() );
    EXPECT_FALSE( report.SceneSettingsHomes.VolumeCreated );
    EXPECT_TRUE( scene.Entities.empty() );
    EXPECT_EQ( report.SceneSettingsHomes.LightsStamped, 0 ) << "no light: the shadow key has nowhere to go";
    EXPECT_FALSE( scene.Settings.value().to_object().value().get( "EnableShadows" ).has_value() );
}

// Every reflected field of PostProcessSettings is a key the step moves — a field added to the grade without
// being added to the step would be silently LEFT in the Settings block, where nothing reads it.
TEST( SceneSettingsHomesMigration, EveryGradeFieldIsMovedAndResolvesToTheValueTheBlockStated )
{
    const auto* type = Reflection::ReflectionRegistry::Get().Find( "PostProcessSettings" );
    ASSERT_NE( type, nullptr );

    Core::PostProcessSettings stated;
    stated.EnableSSAO           = false;
    stated.GIIntensity          = 7.25f;
    stated.SSRIntensity         = 0.3f;
    stated.AutoExposureMax      = 3.0f;
    stated.BloomThreshold       = 0.7f;
    stated.LensDispersion       = 1.1f;
    stated.LensFlareStreakAngle = 33.0f;
    stated.LensFlareChromaShift = 0.9f;

    auto scene        = Parse( kSceneV32 );
    auto block        = Reflection::SerializeReflected( *type, &stated );
    block["Gravity"]  = rfl::Generic( 700.0 );
    scene.Settings    = rfl::Generic( block );
    const auto report = Migration::MigrateScene( scene, "", "" );
    ASSERT_TRUE( report.Refused.empty() );
    EXPECT_EQ( static_cast<std::size_t>( report.SceneSettingsHomes.PostKeysMoved ), type->Fields.size() );
    EXPECT_EQ( scene.Settings.value().to_object().value().size(), 1u ) << "only Gravity stays";

    const entt::registry             reg      = RegistryOf( scene );
    const Graphic::FinalViewSettings resolved = Graphic::ResolveViewSettings( reg, glm::vec3( 1.0e6f ) );
    EXPECT_EQ( Common::Json::Write( Reflection::SerializeReflected( *type, &resolved.Post ) ),
               Common::Json::Write( Reflection::SerializeReflected( *type, &stated ) ) )
         << "the migrated scene must render with exactly the grade its Settings block stated";
}

TEST( SceneSettingsHomesMigration, TheMigratedSceneResolvesToThePreviousValues )
{
    auto scene = Parse( kSceneV32 );
    ASSERT_TRUE( Migration::MigrateScene( scene, "", "" ).Refused.empty() );
    const entt::registry reg = RegistryOf( scene );
    const auto           v   = Graphic::ResolveViewSettings( reg, std::nullopt );
    EXPECT_EQ( v.Post.Exposure, 2.5f );
    EXPECT_EQ( v.Post.Gamma, 2.4f );
    EXPECT_EQ( v.Post.WhitePoint, 11.0f );
    EXPECT_EQ( v.Post.AutoExposureKey, 0.21f );
    EXPECT_EQ( v.Post.BloomIntensity, 1.3f );
    EXPECT_EQ( v.Post.SSRMaxDistance, 2500.0f );
    EXPECT_EQ( v.Post.LensFlareTint, glm::vec3( 0.5f, 0.25f, 1.0f ) );
    EXPECT_EQ( v.Post.GlobalIllumination, Core::GIMode::RSM );
    EXPECT_TRUE( v.Post.EnableBloom );
    EXPECT_TRUE( v.Post.AutoExposure );
    EXPECT_EQ( v.Post.BloomThreshold, Core::PostProcessSettings{}.BloomThreshold ) << "unstated = old default";
    EXPECT_FALSE( v.Shadows.Enabled );
    EXPECT_EQ( v.Shadows.Bias, 0.0125f );
    EXPECT_EQ( v.Shadows.CascadeSplitLambda, 0.85f );
}

// ── The blend ────────────────────────────────────────────────────────────────────────────────────────────

TEST( PostProcessVolume, NoVolumeIsTheDefaultGrade )
{
    const entt::registry reg;
    const auto           v = Graphic::ResolveViewSettings( reg, glm::vec3( 0.0f ) );
    EXPECT_EQ( v.Post.Exposure, Core::PostProcessSettings{}.Exposure );
    EXPECT_FALSE( v.Post.EnableBloom );
    EXPECT_TRUE( v.Shadows.Enabled );
}

TEST( PostProcessVolume, AnUnboundVolumeAppliesAnywhereAndWithoutACamera )
{
    entt::registry reg;
    AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 0.0f, 1.0f, 3.0f, true ) );
    EXPECT_EQ( Graphic::ResolveViewSettings( reg, glm::vec3( 1.0e7f ) ).Post.Exposure, 3.0f );
    EXPECT_EQ( Graphic::ResolveViewSettings( reg, std::nullopt ).Post.Exposure, 3.0f );
}

TEST( PostProcessVolume, TheHigherPriorityWinsWhateverTheRegistryOrder )
{
    for ( const bool highFirst : { true, false } )
    {
        entt::registry reg;
        if ( highFirst )
            AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 5.0f, 1.0f, 4.0f, false ) );
        AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 1.0f, 1.0f, 2.0f, true ) );
        if ( !highFirst )
            AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 5.0f, 1.0f, 4.0f, false ) );
        const auto v = Graphic::ResolveViewSettings( reg, glm::vec3( 0.0f ) );
        EXPECT_EQ( v.Post.Exposure, 4.0f ) << "highFirst=" << highFirst;
        EXPECT_FALSE( v.Post.EnableBloom ) << "highFirst=" << highFirst;
    }
}

TEST( PostProcessVolume, BlendWeightLerpsValuesAndTakesSwitchesWhole )
{
    entt::registry reg;
    AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 0.0f, 1.0f, 2.0f, false ) );
    AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 1.0f, 0.25f, 6.0f, true ) );
    const auto v = Graphic::ResolveViewSettings( reg, glm::vec3( 0.0f ) );
    EXPECT_FLOAT_EQ( v.Post.Exposure, 3.0f ); // 2 * 0.75 + 6 * 0.25
    EXPECT_TRUE( v.Post.EnableBloom );

    entt::registry zero;
    AddVolume( zero, glm::vec3( 0.0f ), Volume( true, 0.0f, 0.0f, 6.0f, true ) );
    EXPECT_FALSE( Graphic::ResolveViewSettings( zero, glm::vec3( 0.0f ) ).Post.EnableBloom )
         << "a volume of weight 0 contributes nothing, not even its switches";
}

TEST( PostProcessVolume, ABoundedVolumeAppliesInsideFadesAcrossItsRadiusAndNotBeyond )
{
    entt::registry             reg;
    ECS::PostProcessVolumeData box = Volume( false, 0.0f, 1.0f, 5.0f, true );
    box.Extent                     = glm::vec3( 100.0f );
    box.BlendRadius                = 200.0f;
    AddVolume( reg, glm::vec3( 1000.0f, 0.0f, 0.0f ), box );
    const float base = Core::PostProcessSettings{}.Exposure;

    EXPECT_EQ( Graphic::ResolveViewSettings( reg, glm::vec3( 1050.0f, 0.0f, 0.0f ) ).Post.Exposure, 5.0f );
    const auto fringe = Graphic::ResolveViewSettings( reg, glm::vec3( 1200.0f, 0.0f, 0.0f ) ); // 100 cm out
    EXPECT_FLOAT_EQ( fringe.Post.Exposure, base * 0.5f + 5.0f * 0.5f );
    const auto outside = Graphic::ResolveViewSettings( reg, glm::vec3( 1400.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( outside.Post.Exposure, base );
    EXPECT_FALSE( outside.Post.EnableBloom );
    EXPECT_EQ( Graphic::ResolveViewSettings( reg, std::nullopt ).Post.Exposure, base )
         << "a view with no camera has no position to be inside anything";
}

TEST( PostProcessVolume, AHiddenVolumeAndAHiddenLightAreSkipped )
{
    entt::registry     reg;
    const entt::entity v = AddVolume( reg, glm::vec3( 0.0f ), Volume( true, 0.0f, 1.0f, 9.0f, true ) );
    reg.emplace<ECS::VisibilityComponent>( v ).Visible = false;

    const entt::entity hidden                                            = reg.create();
    reg.emplace<ECS::TransformComponent>( hidden ).Translation           = glm::vec3( 0.0f, -1.0f, 0.0f );
    reg.emplace<ECS::DirectionLightComponent>( hidden ).Data.CastShadows = false;
    reg.emplace<ECS::VisibilityComponent>( hidden ).Visible              = false;

    const auto r = Graphic::ResolveViewSettings( reg, glm::vec3( 0.0f ) );
    EXPECT_EQ( r.Post.Exposure, Core::PostProcessSettings{}.Exposure );
    EXPECT_TRUE( r.Shadows.Enabled );
}

TEST( PostProcessVolume, TheSceneSchemaIsTheOneThisStepStamps )
{
    EXPECT_EQ( Core::kSceneVersion, 36 );
    EXPECT_EQ( Migration::kSceneVersionSceneSettingsHomes, Core::kSceneVersion );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

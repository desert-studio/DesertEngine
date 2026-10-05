// THE LEVEL SEQUENCE ACTOR, without a Scene or a GPU: the `.dseq` text, the entity host the system applies
// through, the per-actor state that turns steps into a view target and log lines, and LoopMode's stored name.

#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Common/Json/Document.hpp>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>
#include <Editor/Panels/Sequencer/LevelMaterialProperties.hpp>

#include "../ClipFixture.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

#include <gtest/gtest.h>

#include <map>
#include <tuple>

#include <string>
#include <vector>

namespace
{
    namespace A   = Desert::Animation;
    namespace T   = Desert::Animation::Timeline;
    namespace ECS = Desert::ECS;
    using Common::Content::AssetGuid;

    constexpr uint64_t kDoorUuid   = 4101;
    constexpr uint64_t kOtherUuid  = 4102;
    constexpr uint64_t kCameraUuid = 4103;

    A::FrameTime At( int32_t tick )
    {
        return A::FrameTime{ A::FrameNumber{ tick }, 0.0F };
    }
    T::TimeStep Step( int32_t tick )
    {
        return T::TimeStep{ At( tick ), At( tick ) };
    }
    T::BindingGuid Guid( uint64_t lo )
    {
        return T::BindingGuid{ AssetGuid{ 7, lo } };
    }
    A::ScalarKey Key( int32_t tick, float value )
    {
        A::ScalarKey key;
        key.Tick   = A::FrameNumber{ tick };
        key.Value  = value;
        key.Interp = A::KeyInterp::Linear;
        return key;
    }

    /// "Door" (entity @p doorLocator) slides X 0 → 100 over ticks 0..100; the Camera Cut looks through
    /// "Camera" on ticks 0..40 and nothing after.
    T::Sequence DoorAndCut( const std::string& doorLocator )
    {
        T::Sequence sequence;
        sequence.Host  = T::SequenceHost::LevelSequence;
        sequence.Start = A::FrameNumber{ 0 };
        sequence.End   = A::FrameNumber{ 100 };
        sequence.Bindings.push_back( T::Binding{ Guid( 1 ), T::BindingKind::Entity, doorLocator, "Door", {} } );
        sequence.Bindings.push_back(
             T::Binding{ Guid( 2 ), T::BindingKind::Entity, std::to_string( kCameraUuid ), "Camera", {} } );
        sequence.Bindings.push_back( T::Binding{ Guid( 3 ), T::BindingKind::Sequence, "", "Cuts", {} } );

        T::Track slide;
        slide.Binding  = Guid( 1 );
        slide.Property = "Translation";
        slide.Kind     = T::TrackKind::Vector;
        T::Section keys;
        keys.Start = A::FrameNumber{ 0 };
        keys.End   = A::FrameNumber{ 100 };
        T::VectorChannel vector;
        vector.X.Keys = { Key( 0, 0.0F ), Key( 100, 100.0F ) };
        keys.Content  = T::Channel{ vector };
        slide.Sections.push_back( std::move( keys ) );
        sequence.Tracks.push_back( std::move( slide ) );

        T::Track cuts;
        cuts.Binding  = Guid( 3 );
        cuts.Property = "CameraCut";
        cuts.Kind     = T::TrackKind::CameraCut;
        T::Section cut;
        cut.Start   = A::FrameNumber{ 0 };
        cut.End     = A::FrameNumber{ 40 };
        cut.Content = T::CameraCutSectionContent{ Guid( 2 ) };
        cuts.Sections.push_back( std::move( cut ) );
        sequence.Tracks.push_back( std::move( cuts ) );
        return sequence;
    }

    entt::entity Spawn( entt::registry& registry, uint64_t uuid )
    {
        const entt::entity entity                           = registry.create();
        registry.emplace<ECS::UUIDComponent>( entity ).UUID = Common::UUID( uuid );
        registry.emplace<ECS::TransformComponent>( entity );
        return entity;
    }

    struct World
    {
        entt::registry registry;
        entt::entity   door   = Spawn( registry, kDoorUuid );
        entt::entity   other  = Spawn( registry, kOtherUuid );
        entt::entity   camera = Spawn( registry, kCameraUuid );
        entt::entity   player = Spawn( registry, 4104 ); ///< the view target before any cut

        World()
        {
            registry.emplace<ECS::CameraComponent>( camera );
            registry.emplace<ECS::CameraComponent>( player );
        }
    };
} // namespace

TEST( LevelSequenceAsset, WriteParseWriteIsTheSameText )
{
    const T::Sequence sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    ASSERT_TRUE( T::Validate( sequence ).IsSuccess() ) << T::Validate( sequence ).GetError();
    const AssetGuid guid{ 0x1234, 0x5678 };

    const auto text = Desert::Assets::LevelSequenceAsset::Write( sequence, guid );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    EXPECT_NE( text.GetValue().find( "\"LevelSequence\"" ), std::string::npos );

    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Guid, guid );
    EXPECT_EQ( parsed.GetValue().Sequence.Bindings.size(), 3U );

    const auto again = Desert::Assets::LevelSequenceAsset::Write( parsed.GetValue().Sequence, guid );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_EQ( again.GetValue(), text.GetValue() );
}

TEST( LevelSequenceAsset, RefusesAnotherHostAndTheNullGuid )
{
    T::Sequence ui  = DoorAndCut( "1" );
    ui.Host         = T::SequenceHost::UIAnimation;
    const auto host = Desert::Assets::LevelSequenceAsset::Write( ui, AssetGuid{ 1, 2 } );
    ASSERT_FALSE( host.IsSuccess() );
    EXPECT_NE( host.GetError().find( "LevelSequence" ), std::string::npos ) << host.GetError();

    EXPECT_FALSE( Desert::Assets::LevelSequenceAsset::Write( DoorAndCut( "1" ), AssetGuid{} ).IsSuccess() );
    EXPECT_FALSE( Desert::Assets::LevelSequenceAsset::Parse( "{}" ).IsSuccess() );
}

TEST( LevelSequencePlayback, MovesTheBoundEntityByItsKeys )
{
    World                             world;
    const T::Sequence                 sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );

    for ( const auto [tick, x] : { std::pair{ 0, 0.0F }, std::pair{ 50, 50.0F }, std::pair{ 100, 100.0F } } )
    {
        const auto step = ECS::StepLevelSequence( world.registry, component, playback, Step( tick ) );
        EXPECT_TRUE( step.Report.Unresolved.empty() );
        EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
        EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, x, 1e-4F )
             << "tick " << tick;
    }
    EXPECT_EQ( world.registry.get<ECS::TransformComponent>( world.other ).Translation.x, 0.0F );
}

TEST( LevelSequencePlayback, ABindingOverrideWinsOverTheLocator )
{
    World                       world;
    const T::Sequence           sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    ECS::LevelSequenceComponent component;
    component.BindingOverrides.push_back( { Guid( 1 ), Common::UUID( kOtherUuid ) } );
    ECS::LevelSequencePlayback playback( sequence );

    (void)ECS::StepLevelSequence( world.registry, component, playback, Step( 50 ) );
    EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.other ).Translation.x, 50.0F, 1e-4F );
    EXPECT_EQ( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 0.0F );
}

// The Details "+" offers the Entity bindings not overridden yet, in the sequence's order; the Sequence
// binding (no object) is never offered, and a binding leaves the list once it has an override.
TEST( LevelSequencePlayback, OverridableBindingsAreTheEntityBindingsWithoutAnOverride )
{
    const T::Sequence           sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    ECS::LevelSequenceComponent component;

    const auto labels = [&]
    {
        std::vector<std::string> names;
        for ( const T::Binding* binding : ECS::OverridableBindings( sequence, component ) )
            names.push_back( binding->Label );
        return names;
    };
    EXPECT_EQ( labels(), ( std::vector<std::string>{ "Door", "Camera" } ) );

    component.BindingOverrides.push_back( { Guid( 1 ), Common::UUID( kOtherUuid ) } );
    EXPECT_EQ( labels(), ( std::vector<std::string>{ "Camera" } ) );
}

TEST( LevelSequencePlayback, CameraCutTakesTheViewAndGivesThePreviousBack )
{
    World                             world;
    const T::Sequence                 sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );
    ECS::LevelSequenceActorState      state;

    const auto during = ECS::StepLevelSequence( world.registry, component, playback, Step( 20 ) );
    if ( !during.CameraCut )
    {
        FAIL() << "the cut section names no camera at tick 20";
    }
    EXPECT_EQ( *during.CameraCut, world.camera );
    EXPECT_EQ( ECS::LevelSequenceViewTarget( state, during, world.player ), std::optional{ world.camera } );
    // Still in the cut: the view is the cut's camera, and the target to give back stays the player's.
    EXPECT_EQ( ECS::LevelSequenceViewTarget( state, during, world.camera ), std::optional{ world.camera } );

    const auto after = ECS::StepLevelSequence( world.registry, component, playback, Step( 60 ) );
    EXPECT_FALSE( after.CameraCut.has_value() );
    EXPECT_EQ( ECS::LevelSequenceViewTarget( state, after, world.camera ), std::optional{ world.player } );
    EXPECT_EQ( ECS::LevelSequenceViewTarget( state, after, world.player ), std::nullopt ); // given back once
}

TEST( LevelSequencePlayback, AnUnknownUuidIsANamedErrorReportedOnce )
{
    World                             world;
    const T::Sequence                 sequence = DoorAndCut( "999999" ); // no entity of this registry
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );
    ECS::LevelSequenceActorState      state;

    const auto first = ECS::StepLevelSequence( world.registry, component, playback, Step( 50 ) );
    ASSERT_EQ( first.Report.Unresolved.size(), 1U );
    EXPECT_EQ( first.Report.Unresolved.front(), "Door" );
    const auto errors = ECS::TakeNewLevelSequenceErrors( state, first );
    ASSERT_EQ( errors.size(), 1U );
    EXPECT_NE( errors.front().find( "'Door'" ), std::string::npos ) << errors.front();

    const auto second = ECS::StepLevelSequence( world.registry, component, playback, Step( 60 ) );
    EXPECT_EQ( second.Report.Unresolved.size(), 1U );
    EXPECT_TRUE( ECS::TakeNewLevelSequenceErrors( state, second ).empty() );
    EXPECT_EQ( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 0.0F );
}

TEST( LevelSequenceComponent, LoopModeIsStoredByName )
{
    const Common::Json::Value stored(
         Common::Json::ObjectBuilder().Set( "Loop", T::LoopMode::PingPong ).Set( "Bad", "Sometimes" ).Build() );
    const Common::Json::Node root = Common::Json::Root( stored );

    const auto name = root.Find( "Loop" );
    if ( !name )
    {
        FAIL() << "the stored object has no Loop field";
    }
    const auto text = name->AsString();
    ASSERT_TRUE( text.IsSuccess() );
    EXPECT_EQ( text.GetValue(), "PingPong" );

    Common::Json::Issues issues;
    T::LoopMode          loop = T::LoopMode::Once;
    root.ReadInto( "Loop", loop, issues );
    EXPECT_TRUE( issues.empty() );
    EXPECT_EQ( loop, T::LoopMode::PingPong );

    T::LoopMode kept = T::LoopMode::Loop;
    root.ReadInto( "Bad", kept, issues );
    EXPECT_EQ( issues.size(), 1U ); // an unknown name is a named issue
    EXPECT_EQ( kept, T::LoopMode::Loop );
}

TEST( LevelSequenceComponent, LoopModeNamesAreTheStoredOnes )
{
    // The Details combo shows ToString; the scene stores the reflected name. One spelling, both ways.
    for ( const T::LoopMode mode : T::kLoopModes )
    {
        const Common::Json::Value stored( Common::Json::ObjectBuilder().Set( "Loop", mode ).Build() );
        const auto                loop = Common::Json::Root( stored ).Find( "Loop" );
        if ( !loop )
        {
            FAIL() << "the stored object has no Loop field";
        }
        const auto text = loop->AsString();
        ASSERT_TRUE( text.IsSuccess() );
        EXPECT_EQ( text.GetValue(), T::ToString( mode ) );
        EXPECT_EQ( T::LoopModeFromString( T::ToString( mode ) ), mode );
    }
    EXPECT_FALSE( T::LoopModeFromString( "Sometimes" ).has_value() );
    EXPECT_STREQ( T::ToString( static_cast<T::LoopMode>( 7 ) ), "Unknown" );
}

// ── THE LEVEL SEQUENCE DOCUMENT (ANIM-LSEQ) ──────────────────────────────────────────────────────────────────
namespace
{
    /// What the Sequencer's document authors: "+ Track → Actor" on the door, Transform keys X 0 at tick 0 and
    /// X 100 at tick 100 — through the document's own edits, not a hand-built sequence.
    T::Sequence AuthoredDoor()
    {
        T::Sequence sequence;
        sequence.Host   = T::SequenceHost::LevelSequence;
        sequence.Start  = A::FrameNumber{ 0 };
        sequence.End    = A::FrameNumber{ 100 };
        const auto door = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
        EXPECT_TRUE( door.IsSuccess() );
        ECS::TransformComponent pose;
        EXPECT_TRUE(
             ECS::SetEntityTransformKey( sequence, door.GetValue(), A::FrameNumber{ 0 }, ECS::EntityPose( pose ) )
                  .IsSuccess() );
        pose.Translation.x = 100.0F;
        EXPECT_TRUE( ECS::SetEntityTransformKey( sequence, door.GetValue(), A::FrameNumber{ 100 },
                                                 ECS::EntityPose( pose ) )
                          .IsSuccess() );
        return sequence;
    }
} // namespace

TEST( LevelSequenceDocument, SavesAndReadsWhatTheComponentPlays )
{
    const T::Sequence authored = AuthoredDoor();
    ASSERT_TRUE( T::Validate( authored ).IsSuccess() ) << T::Validate( authored ).GetError();
    const auto text = Desert::Assets::LevelSequenceAsset::Write( authored, AssetGuid{ 1, 2 } );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( parsed.GetValue().Sequence );
    const auto step = ECS::StepLevelSequence( world.registry, component, playback, Step( 50 ) );
    EXPECT_TRUE( step.Report.Unresolved.empty() );
    EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
    EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 50.0F, 1e-3F );
}

TEST( LevelSequenceDocument, TheActorBindingSurvivesTheSaveAndNamesTheEntityByUuid )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  again    = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door (again)" );
    ASSERT_TRUE( again.IsSuccess() );
    EXPECT_EQ( sequence.Bindings.size(), 1U ) << "binding one entity twice is one binding";

    const auto text   = Desert::Assets::LevelSequenceAsset::Write( sequence, AssetGuid{ 1, 2 } );
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    ASSERT_EQ( parsed.GetValue().Sequence.Bindings.size(), 1U );
    const T::Binding& binding = parsed.GetValue().Sequence.Bindings.front();
    EXPECT_EQ( binding.Guid, again.GetValue() );
    EXPECT_EQ( binding.Kind, T::BindingKind::Entity );
    EXPECT_EQ( binding.Locator, std::to_string( kDoorUuid ) );

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequenceEntityHost      host( world.registry, component );
    const auto                        resolved = host.Resolve( binding );
    if ( !resolved )
        FAIL() << "the binding resolved to nothing";
    const auto& bound = *resolved;
    EXPECT_EQ( static_cast<entt::entity>( static_cast<uint32_t>( bound.Handle ) ), world.door );
}

TEST( LevelSequenceDocument, PreviewPosesTheSceneAndClosingGivesItBack )
{
    const T::Sequence sequence = AuthoredDoor();
    World             world;
    world.registry.get<ECS::TransformComponent>( world.door ).Translation = { 7.0F, 1.0F, 2.0F };

    ECS::LevelSequencePreview preview;
    (void)preview.Scrub( world.registry, sequence, A::FrameNumber{ 50 } );
    EXPECT_TRUE( preview.Active() );
    EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 50.0F, 1e-3F );
    (void)preview.Scrub( world.registry, sequence, A::FrameNumber{ 100 } );
    EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 100.0F, 1e-3F );

    preview.Restore( world.registry );
    EXPECT_FALSE( preview.Active() );
    const auto& restored = world.registry.get<ECS::TransformComponent>( world.door ).Translation;
    EXPECT_EQ( restored.x, 7.0F );
    EXPECT_EQ( restored.y, 1.0F );
    EXPECT_EQ( restored.z, 2.0F );
    EXPECT_FALSE( world.registry.has<ECS::VisibilityComponent>( world.door ) );
}

TEST( LevelSequenceDocument, AnOverlappingCameraCutLeavesTheSequenceAsItWas )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  camera   = ECS::AddEntityBinding( sequence, Common::UUID( kCameraUuid ), "Camera" );
    ASSERT_TRUE(
         ECS::AddCameraCut( sequence, camera.GetValue(), A::FrameNumber{ 0 }, A::FrameNumber{ 40 } ).IsSuccess() );
    const size_t tracks = sequence.Tracks.size();
    EXPECT_FALSE( ECS::AddCameraCut( sequence, camera.GetValue(), A::FrameNumber{ 20 }, A::FrameNumber{ 60 } )
                       .IsSuccess() );
    EXPECT_EQ( sequence.Tracks.size(), tracks );
    EXPECT_EQ( sequence.Tracks.back().Sections.size(), 1U );
}

TEST( LevelSequenceDocument, AVisibilityTrackHidesTheActorFromItsKeyOn )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  added    = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( added.IsSuccess() );
    const T::BindingGuid door = added.GetValue();
    ASSERT_TRUE( ECS::AddVisibilityTrack( sequence, door, true ).IsSuccess() );
    EXPECT_FALSE( ECS::AddVisibilityTrack( sequence, door, true ).IsSuccess() )
         << "one Visibility track per actor";
    ASSERT_TRUE( ECS::SetVisibilityKey( sequence, door, A::FrameNumber{ 60 }, false ).IsSuccess() );
    ASSERT_TRUE( T::Validate( sequence ).IsSuccess() ) << T::Validate( sequence ).GetError();

    const auto keys = ECS::VisibilityKeys( sequence, door );
    ASSERT_EQ( keys.size(), 2U ) << "the start key (current value) and the hidden key";
    EXPECT_EQ( keys[0].Tick.Value, 0 );
    EXPECT_TRUE( keys[0].Visible );
    EXPECT_EQ( keys[1].Tick.Value, 60 );
    EXPECT_FALSE( keys[1].Visible );

    // Through the .dseq text, as the component plays it.
    const auto text = Desert::Assets::LevelSequenceAsset::Write( sequence, AssetGuid{ 1, 2 } );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( parsed.GetValue().Sequence );
    for ( const auto [tick, visible] :
          { std::pair{ 0, true }, std::pair{ 59, true }, std::pair{ 60, false }, std::pair{ 100, false } } )
    {
        const auto step = ECS::StepLevelSequence( world.registry, component, playback, Step( tick ) );
        EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
        ASSERT_TRUE( world.registry.has<ECS::VisibilityComponent>( world.door ) ) << "tick " << tick;
        EXPECT_EQ( world.registry.get<ECS::VisibilityComponent>( world.door ).Visible, visible )
             << "tick " << tick;
    }
    EXPECT_FALSE( world.registry.has<ECS::VisibilityComponent>( world.other ) );
}

// The suite's stand-in for MeshECSSystem's per-entity slot instances: (entity, slot, parameter) → override.
// The "asset" is a separate value no Set may reach — what the material asset says before and after.
struct FakeMaterialSlots
{
    std::map<std::tuple<entt::entity, uint32_t, std::string>, glm::vec4> Overrides;
    entt::entity                                                         Owner = entt::null;
    uint32_t                                                             Slots = 1;

    ECS::LevelSequenceMaterialSlots Access()
    {
        ECS::LevelSequenceMaterialSlots access;
        access.Get = [this]( entt::registry&, entt::entity entity,
                             const ECS::LevelSequenceMaterialParameter& parameter ) -> std::optional<glm::vec4>
        {
            const auto at = Overrides.find( { entity, parameter.Slot, parameter.Name } );
            return at != Overrides.end() ? std::optional<glm::vec4>( at->second ) : std::nullopt;
        };
        access.Set = [this]( entt::registry&, entt::entity entity,
                             const ECS::LevelSequenceMaterialParameter& parameter,
                             const std::optional<glm::vec4>&            value )
        {
            if ( entity != Owner || parameter.Slot >= Slots )
                return false;
            if ( value )
                Overrides[{ entity, parameter.Slot, parameter.Name }] = *value;
            else
                Overrides.erase( { entity, parameter.Slot, parameter.Name } );
            return true;
        };
        return access;
    }
};

TEST( LevelSequenceDocument, AMaterialParameterTrackDrivesTheActorsSlotOverrideNotTheAsset )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  added    = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( added.IsSuccess() );
    const T::BindingGuid                      door = added.GetValue();
    const ECS::LevelSequenceMaterialParameter glow{ 0, "Emissive" };
    const ECS::LevelSequenceMaterialParameter tint{ 0, "BaseColor" };
    EXPECT_EQ( ECS::ParseLevelSequenceMaterialProperty( ECS::LevelSequenceMaterialProperty( glow ) ), glow );
    EXPECT_FALSE( ECS::ParseLevelSequenceMaterialProperty( "Material.x.Emissive" ) );

    const glm::vec4 asset( 0.25F, 0.0F, 0.0F, 0.0F ); // what the material asset says; nothing may write it
    ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, glow, T::TrackKind::Float, asset ).IsSuccess() );
    EXPECT_FALSE( ECS::AddMaterialParameterTrack( sequence, door, glow, T::TrackKind::Float, asset ).IsSuccess() )
         << "one track per (slot, parameter)";
    EXPECT_FALSE( ECS::AddMaterialParameterTrack( sequence, door, tint, T::TrackKind::Bool, asset ).IsSuccess() );
    ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, tint, T::TrackKind::Vector, glm::vec4( 1.0F ) )
                      .IsSuccess() );
    ASSERT_TRUE( ECS::SetMaterialParameterKey( sequence, door, glow, A::FrameNumber{ 0 }, glm::vec4( 0.0F ) )
                      .IsSuccess() );
    ASSERT_TRUE( ECS::SetMaterialParameterKey( sequence, door, glow, A::FrameNumber{ 60 }, glm::vec4( 1.0F ) )
                      .IsSuccess() );
    ASSERT_TRUE( ECS::SetMaterialParameterKey( sequence, door, tint, A::FrameNumber{ 60 },
                                               glm::vec4( 0.0F, 0.5F, 1.0F, 0.0F ) )
                      .IsSuccess() );
    ASSERT_TRUE( T::Validate( sequence ).IsSuccess() ) << T::Validate( sequence ).GetError();

    // Through the .dseq text, as the component plays it.
    const auto text = Desert::Assets::LevelSequenceAsset::Write( sequence, AssetGuid{ 1, 2 } );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    World             world;
    FakeMaterialSlots slots;
    slots.Owner                                     = world.door;
    slots.Overrides[{ world.door, 0, "BaseColor" }] = glm::vec4( 1.0F, 1.0F, 1.0F, 0.75F ); // an alpha of its own
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( parsed.GetValue().Sequence );

    const auto step =
         ECS::StepLevelSequence( world.registry, component, playback, Step( 30 ), {}, slots.Access() );
    EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
    const auto emissive = slots.Overrides.find( { world.door, 0U, std::string( "Emissive" ) } );
    ASSERT_NE( emissive, slots.Overrides.end() );
    EXPECT_FLOAT_EQ( emissive->second.x, 0.5F ) << "keys 0 @0 and 1 @60 → 0.5 @30";
    const glm::vec4 color = slots.Overrides.at( { world.door, 0U, std::string( "BaseColor" ) } );
    EXPECT_FLOAT_EQ( color.y, 0.75F );
    EXPECT_FLOAT_EQ( color.w, 0.75F ) << "the unkeyed alpha keeps the slot's own";
    EXPECT_EQ( asset, glm::vec4( 0.25F, 0.0F, 0.0F, 0.0F ) );

    // No slot access, or no such slot: refused by name, never skipped in silence.
    ECS::LevelSequencePlayback blind( parsed.GetValue().Sequence );
    EXPECT_EQ( ECS::StepLevelSequence( world.registry, component, blind, Step( 30 ) ).Refusals.size(), 2U );
    slots.Slots = 0;
    EXPECT_EQ( ECS::StepLevelSequence( world.registry, component, blind, Step( 30 ), {}, slots.Access() )
                    .Refusals.size(),
               2U );

    // The preview gives the overrides back: the one the slot had, and none where it had none.
    slots.Slots     = 1;
    slots.Overrides = { { { world.door, 0, "BaseColor" }, glm::vec4( 1.0F, 1.0F, 1.0F, 0.75F ) } };
    ECS::LevelSequencePreview preview;
    (void)preview.Scrub( world.registry, parsed.GetValue().Sequence, A::FrameNumber{ 30 }, {}, slots.Access() );
    EXPECT_EQ( slots.Overrides.size(), 2U );
    preview.Restore( world.registry );
    ASSERT_EQ( slots.Overrides.size(), 1U ) << "the Emissive override the preview added is dropped";
    EXPECT_EQ( slots.Overrides.begin()->second, glm::vec4( 1.0F, 1.0F, 1.0F, 0.75F ) );
}

TEST( LevelSequenceDocument, AnAnimationTrackPosesTheBoundEntitysSkeleton )
{
    // A two-bone chain; the clip holds "child" at +5 on Y for one second.
    std::vector<A::BoneInfo> bones( 2 );
    bones[0].Name               = "root";
    bones[0].LocalBindTransform = glm::mat4( 1.0F );
    bones[1].Name               = "child";
    bones[1].ParentBoneID       = 0U;
    bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 1.0F, 0.0F ) );
    A::Skeleton skeleton( std::move( bones ) );
    skeleton.RecomputeOffsetMatrices();
    const A::AnimationClip walk = ClipFixture::StaticBoneClip(
         "Walk", A::FrameNumber{ A::PROJECT_TICK_RATE.Numerator }, "child", glm::vec3( 0.0F, 5.0F, 0.0F ) );
    const AssetGuid walkGuid{ 0xA11, 0xCE };

    World world;
    auto& animation =
         world.registry.emplace<ECS::AnimationComponent>( world.door, std::make_unique<A::Animator>( skeleton ) );
    ASSERT_TRUE( animation.Playing );

    T::Sequence sequence;
    sequence.Host   = T::SequenceHost::LevelSequence;
    sequence.Start  = A::FrameNumber{ 0 };
    sequence.End    = A::FrameNumber{ 100 };
    const auto door = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( door.IsSuccess() );
    const auto added = ECS::AddAnimationSection( sequence, door.GetValue(), walkGuid, A::FrameNumber{ 0 },
                                                 A::FrameNumber{ 100 }, true );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();

    const ECS::LevelSequenceClipSource clips = [&]( const AssetGuid& guid ) -> const A::AnimationClip*
    { return guid == walkGuid ? &walk : nullptr; };

    ECS::LevelSequencePreview preview;
    const auto                step = preview.Scrub( world.registry, sequence, A::FrameNumber{ 10 }, clips );
    EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();

    // The Skeletal timeline's path, by hand: the same clip at the same tick.
    A::Animator reference( skeleton );
    reference.Play( walk );
    reference.SetTick( A::FrameTime{ A::FrameNumber{ 10 }, 0.0F } );

    EXPECT_EQ( animation.Animator->GetCurrentClip(), &walk );
    EXPECT_FALSE( animation.Playing ) << "the sequence owns the playhead while it poses the entity";
    const glm::mat4 posed = animation.Animator->GetPose().Matrices[1];
    EXPECT_NEAR( posed[3].y, reference.GetPose().Matrices[1][3].y, 1e-4F );
    EXPECT_GT( std::abs( posed[3].y ), 1.0F ) << "the child is at the clip's +5, not at its bind +1";

    // No clip source: the section is refused by name, never skipped in silence.
    ECS::LevelSequencePreview blind;
    const auto                refused = blind.Scrub( world.registry, sequence, A::FrameNumber{ 10 } );
    ASSERT_EQ( refused.Refusals.size(), 1U );
    EXPECT_NE( refused.Refusals.front().find( "no such clip" ), std::string::npos ) << refused.Refusals.front();

    preview.Restore( world.registry );
    EXPECT_TRUE( animation.Playing ) << "closing the preview gives the Animation component its playback back";
}

// ── KEY EDITING ON THE LEVEL SEQUENCE (ANIM-FIX2): move / delete / Auto Key, on the model, no UI ─────────────
TEST( LevelSequenceKeys, MoveCarriesEveryLaneAndRefusesAnOccupiedTick )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  door     = sequence.Bindings.front().Guid;
    ASSERT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 2U );

    // Onto the other key: refused, and the sequence is exactly as it was.
    const T::Sequence before = sequence;
    EXPECT_FALSE( ECS::MoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 0 } }, 100 ).IsSuccess() );
    EXPECT_EQ( sequence.Revision, before.Revision );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).front().Value, 0 );

    // Out of the playback range: refused.
    EXPECT_FALSE( ECS::MoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 100 } }, 10 ).IsSuccess() );

    // The 100 key to 60: every lane goes with it, and the door evaluates X 100 there.
    ASSERT_TRUE( ECS::MoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 100 } }, -40 ).IsSuccess() );
    const auto ticks = ECS::EntityTransformKeyTicks( sequence, door );
    ASSERT_EQ( ticks.size(), 2U );
    EXPECT_EQ( ticks[1].Value, 60 );
    EXPECT_GT( sequence.Revision, before.Revision );

    // A selection moved by less than its own spread: both keys move together.
    ASSERT_TRUE( ECS::MoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 0 }, A::FrameNumber{ 60 } }, 30 )
                      .IsSuccess() );
    const auto shifted = ECS::EntityTransformKeyTicks( sequence, door );
    ASSERT_EQ( shifted.size(), 2U );
    EXPECT_EQ( shifted[0].Value, 30 );
    EXPECT_EQ( shifted[1].Value, 90 );
}

TEST( LevelSequenceKeys, DeleteRemovesThePoseAndRefusesAMissingKeyWhole )
{
    T::Sequence       sequence = AuthoredDoor();
    const auto        door     = sequence.Bindings.front().Guid;
    const T::Sequence before   = sequence;
    EXPECT_FALSE( ECS::RemoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 0 }, A::FrameNumber{ 7 } } )
                       .IsSuccess() );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 2U ) << "a refusal must change nothing";
    EXPECT_EQ( sequence.Revision, before.Revision );

    ASSERT_TRUE( ECS::RemoveEntityTransformKeys( sequence, door, { A::FrameNumber{ 100 } } ).IsSuccess() );
    const auto ticks = ECS::EntityTransformKeyTicks( sequence, door );
    ASSERT_EQ( ticks.size(), 1U );
    EXPECT_EQ( ticks[0].Value, 0 );
}

TEST( LevelSequenceKeys, AutoKeyWritesOnePoseKeyOnTheReleaseOfAGestureThatMovedTheActor )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  door     = sequence.Bindings.front().Guid;
    World       world;
    ECS::LevelSequenceAutoKey autoKey;
    const A::FrameNumber      at{ 40 };
    const auto                observe = [&]( const bool held ) -> uint32_t
    {
        const auto keyed = autoKey.Observe( world.registry, sequence, at, held );
        EXPECT_TRUE( keyed.IsSuccess() );
        return keyed.IsSuccess() ? keyed.GetValue() : 999U;
    };

    // A gesture that moves nothing keys nothing.
    EXPECT_EQ( observe( true ), 0U );
    EXPECT_TRUE( autoKey.Releasing( false ) );
    EXPECT_EQ( observe( false ), 0U );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 2U );

    // Press, drag (nothing written while held), release: one key at the playhead with the live pose.
    EXPECT_EQ( observe( true ), 0U );
    world.registry.get<ECS::TransformComponent>( world.door ).Translation.x = 777.0F;
    EXPECT_EQ( observe( true ), 0U );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 2U ) << "nothing is keyed mid-gesture";
    EXPECT_EQ( observe( false ), 1U );
    const auto ticks = ECS::EntityTransformKeyTicks( sequence, door );
    ASSERT_EQ( ticks.size(), 3U );
    EXPECT_EQ( ticks[1].Value, 40 );

    // Reset mid-gesture (REC switched off): the release keys nothing.
    EXPECT_EQ( observe( true ), 0U );
    world.registry.get<ECS::TransformComponent>( world.door ).Translation.x = 5.0F;
    autoKey.Reset();
    EXPECT_FALSE( autoKey.Releasing( false ) );
    EXPECT_EQ( observe( false ), 0U );
}

namespace
{
    /// A recorded half of the gesture: whatever it captured is put back on Undo and re-applied on Redo.
    template <typename Value>
    class Restore final : public Desert::Editor::ICommand
    {
    public:
        Restore( Value& live, Value before ) : m_Live( live ), m_Before( std::move( before ) ), m_After( live )
        {
        }
        bool Undo() override
        {
            m_Live = m_Before;
            return true;
        }
        bool Redo() override
        {
            m_Live = m_After;
            return true;
        }

    private:
        Value& m_Live;
        Value  m_Before;
        Value  m_After;
    };
} // namespace

// UE: an actor dragged with Auto Key on is ONE FScopedTransaction — the move and its key. The editor records
// the move (the gizmo's entry) and the key (the Sequencer's) separately; `JoinFollowUp` makes them one Ctrl+Z,
// and only when nothing else was recorded between them.
TEST( LevelSequenceKeys, AutoKeyedGizmoReleaseIsOneUndoStep )
{
    auto& history = Desert::Editor::CommandHistory::Get();
    history.Clear();
    T::Sequence               sequence = AuthoredDoor();
    const auto                door     = sequence.Bindings.front().Guid;
    World                     world;
    auto&                     moved = world.registry.get<ECS::TransformComponent>( world.door ).Translation;
    ECS::LevelSequenceAutoKey autoKey;
    const A::FrameNumber      at{ 40 };

    // The gesture: press, drag, release — the gizmo pushes the move and remembers the revision it stood at.
    ASSERT_TRUE( autoKey.Observe( world.registry, sequence, at, true ).IsSuccess() );
    const glm::vec3 before = moved;
    moved.x                = 777.0F;
    history.PushCommand( std::make_unique<Restore<glm::vec3>>( moved, before ) );
    const uint64_t move = history.Revision();

    // The Sequencer's release frame: its undo step opens, the key is written, the step closes.
    const uint64_t    opened  = history.Revision();
    const T::Sequence unkeyed = sequence;
    const auto        keyed   = autoKey.Observe( world.registry, sequence, at, false );
    ASSERT_TRUE( keyed.IsSuccess() );
    ASSERT_EQ( keyed.GetValue(), 1U );
    history.PushCommand( std::make_unique<Restore<T::Sequence>>( sequence, unkeyed ) );
    ASSERT_TRUE( history.JoinFollowUp( move, opened ) );

    // One Ctrl+Z: the actor is back AND the key is gone.
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( moved.x, before.x );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 2U );
    EXPECT_FALSE( history.Undo() ) << "the move and its key were one entry";
    // One Ctrl+Y: both come back.
    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( moved.x, 777.0F );
    EXPECT_EQ( ECS::EntityTransformKeyTicks( sequence, door ).size(), 3U );

    // Anything recorded between the move and the key's step keeps them apart.
    history.Clear();
    history.PushCommand( std::make_unique<Restore<glm::vec3>>( moved, before ) );
    const uint64_t lone  = history.Revision();
    float          other = 0.0F;
    history.PushCommand( std::make_unique<Restore<float>>( other, 1.0F ) );
    const uint64_t late = history.Revision();
    history.PushCommand( std::make_unique<Restore<T::Sequence>>( sequence, unkeyed ) );
    EXPECT_FALSE( history.JoinFollowUp( lone, late ) );
    // A step that pushed nothing joins nothing either.
    EXPECT_FALSE( history.JoinFollowUp( history.Revision(), history.Revision() ) );
    history.Clear();
}

// UE: "+ Track ▸ Material Parameter" and every key on it are one FScopedTransaction each. The Sequencer wraps
// each in the SAME step the other level edits use (ScopedSequenceEdit over the document's SequenceOwner), so
// the add and the key are two Ctrl+Z, and each Ctrl+Z takes back exactly its own.
TEST( LevelSequenceMaterialUndo, AddingAMaterialParameterTrackAndKeyingItAreOneUndoStepEach )
{
    namespace Ed  = Desert::Editor;
    auto& history = Ed::CommandHistory::Get();
    history.Clear();
    T::Sequence                               sequence = AuthoredDoor();
    const auto                                door     = sequence.Bindings.front().Guid;
    const ECS::LevelSequenceMaterialParameter roughness{ 0, "Roughness" };
    const ECS::LevelSequenceMaterialParameter albedo{ 1, "Albedo" };
    const size_t                              tracksBefore = sequence.Tracks.size();

    Ed::SequenceOwner owner;
    owner.Identity = &sequence;
    owner.Resolve  = [&sequence]() -> T::Sequence* { return &sequence; };
    owner.Volatile = false;
    owner.Name     = "Level Sequence";
    Ed::SequenceEditTransaction transaction;

    {
        const Ed::ScopedSequenceEdit step( transaction, owner );
        ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, roughness, T::TrackKind::Float,
                                                     glm::vec4( 0.25F, 0.0F, 0.0F, 0.0F ) )
                          .IsSuccess() );
    }
    ASSERT_EQ( history.UndoStack().size(), 1U ) << "the add is one step";
    {
        const Ed::ScopedSequenceEdit step( transaction, owner );
        ASSERT_TRUE( ECS::SetMaterialParameterKey( sequence, door, roughness, A::FrameNumber{ 40 },
                                                   glm::vec4( 0.75F, 0.0F, 0.0F, 0.0F ) )
                          .IsSuccess() );
    }
    ASSERT_EQ( history.UndoStack().size(), 2U ) << "the key is one more step";
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } )->x, 0.75F );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 20 } )->x, 0.5F )
         << "Linear between the start key and the new one";
    ASSERT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, roughness ).size(), 2U );

    // A vector track in the same document: its row reads .xyz and lists the merged X/Y/Z key ticks once each.
    {
        const Ed::ScopedSequenceEdit step( transaction, owner );
        ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, albedo, T::TrackKind::Vector,
                                                     glm::vec4( 0.1F, 0.2F, 0.3F, 1.0F ) )
                          .IsSuccess() );
    }
    const auto rows = ECS::MaterialParameterTracks( sequence, door );
    ASSERT_EQ( rows.size(), 2U );
    EXPECT_EQ( rows[0].first, roughness );
    EXPECT_EQ( rows[1].first, albedo );
    EXPECT_EQ( rows[1].second, T::TrackKind::Vector );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, albedo, A::FrameNumber{ 70 } )->z, 0.3F );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, albedo ).size(), 1U );

    // Ctrl+Z ×3: the vector track goes, then only the key (the start key stays), then the scalar track.
    ASSERT_TRUE( history.Undo() );
    EXPECT_FALSE( ECS::HasMaterialParameterTrack( sequence, door, albedo ) );
    ASSERT_TRUE( history.Undo() );
    ASSERT_TRUE( ECS::HasMaterialParameterTrack( sequence, door, roughness ) );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, roughness ).size(), 1U );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } )->x, 0.25F );
    ASSERT_TRUE( history.Undo() );
    EXPECT_FALSE( ECS::HasMaterialParameterTrack( sequence, door, roughness ) );
    EXPECT_FALSE( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ).has_value() );
    EXPECT_EQ( sequence.Tracks.size(), tracksBefore );
    EXPECT_FALSE( history.Undo() ) << "nothing else was recorded";

    // Ctrl+Y ×2: the track, then its key, each by value.
    ASSERT_TRUE( history.Redo() );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } )->x, 0.25F );
    ASSERT_TRUE( history.Redo() );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } )->x, 0.75F );
    history.Clear();
}

// UE: a Material Parameter key's value is edited by the track row's field, and the control channel reaches that
// field as a property of the Level Sequence document ("<actor>.<slot>.<parameter>"). `set` resolves the name
// against the census and keys at the playhead through the row's setter: one key with the sent value, one undo
// step.
TEST( LevelSequenceMaterialProperties, SetKeysTheTrackAtThePlayheadAsOneUndoStep )
{
    namespace Ed  = Desert::Editor;
    namespace LM  = Desert::Editor::LevelMaterialEdit;
    auto& history = Ed::CommandHistory::Get();
    history.Clear();
    T::Sequence                               sequence = AuthoredDoor();
    const auto                                door     = sequence.Bindings.front().Guid;
    const ECS::LevelSequenceMaterialParameter blend{ 0, "Blend" };
    const ECS::LevelSequenceMaterialParameter tint{ 0, "TintB" };
    ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, blend, T::TrackKind::Float, glm::vec4( 0.0F ) )
                      .IsSuccess() );
    ASSERT_TRUE( ECS::AddMaterialParameterTrack( sequence, door, tint, T::TrackKind::Vector,
                                                 glm::vec4( 0.1F, 0.2F, 0.3F, 0.0F ) )
                      .IsSuccess() );
    const std::vector<LM::Schema> schema{
         LM::Schema{ door, blend, LM::SlotLabel( 0, "MP_Default" ), "Blend", false, 0.0F, 1.0F },
         LM::Schema{ door, tint, LM::SlotLabel( 0, "MP_Default" ), "Tint B", true, std::nullopt, std::nullopt } };

    // The census: one property per track, named <actor>.<slot>.<parameter>, grouped under the material's name.
    const A::FrameNumber playhead{ 75 };
    const auto           census = LM::Describe( sequence, playhead, schema );
    ASSERT_EQ( census.size(), 2U );
    EXPECT_EQ( census[0].Name, "Door.0.Blend" );
    EXPECT_EQ( census[0].Group, "Door ▸ Slot 0 (MP_Default)" );
    EXPECT_EQ( census[0].Components, 1 );
    EXPECT_EQ( census[0].Max, std::optional<float>( 1.0F ) );
    EXPECT_EQ( census[1].Name, "Door.0.TintB" );
    EXPECT_EQ( census[1].Type, "color" );
    EXPECT_EQ( census[1].Components, 3 );
    EXPECT_FLOAT_EQ( census[1].Value[2], 0.3F );
    EXPECT_EQ( LM::SlotLabel( 1, "" ), "Slot 1" );

    // Refusals say why: an unknown track, a vector for a scalar, a value the slider cannot reach.
    EXPECT_FALSE( LM::Resolve( sequence, schema, "Door.0.Roughness", { 0.5F } ).IsSuccess() );
    EXPECT_FALSE( LM::Resolve( sequence, schema, "Door.0.Blend", { 0.5F, 0.5F, 0.5F } ).IsSuccess() );
    EXPECT_FALSE( LM::Resolve( sequence, schema, "Door.0.Blend", { 1.5F } ).IsSuccess() );

    Ed::SequenceOwner owner;
    owner.Identity = &sequence;
    owner.Resolve  = [&sequence]() -> T::Sequence* { return &sequence; };
    owner.Volatile = false;
    owner.Name     = "Level Sequence";
    Ed::SequenceEditTransaction transaction;

    const auto write = LM::Resolve( sequence, schema, "Door.0.Blend", { 1.0F } );
    ASSERT_TRUE( write.IsSuccess() ) << write.GetError();
    EXPECT_EQ( write.GetValue().Binding, door );
    EXPECT_EQ( write.GetValue().Parameter, blend );
    ASSERT_TRUE( LM::Key( sequence, transaction, owner, write.GetValue().Binding, write.GetValue().Parameter,
                          playhead, write.GetValue().Value )
                      .IsSuccess() );
    ASSERT_EQ( history.UndoStack().size(), 1U ) << "the set is one step";
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, blend ).size(), 2U );
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, blend, playhead )->x, 1.0F );
    EXPECT_FLOAT_EQ( LM::Describe( sequence, playhead, schema )[0].Value[0], 1.0F )
         << "the census reads the keyed value back at the playhead";

    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, blend ).size(), 1U ) << "Ctrl+Z takes the key back";
    EXPECT_FLOAT_EQ( ECS::MaterialParameterAt( sequence, door, blend, playhead )->x, 0.0F );
    EXPECT_FALSE( history.Undo() ) << "nothing else was recorded";
    history.Clear();
}

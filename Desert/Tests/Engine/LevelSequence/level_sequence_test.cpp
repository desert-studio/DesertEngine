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
#include <optional>
#include <stdexcept>
#include <tuple>

#include <string>
#include <vector>

namespace
{
    namespace A   = Desert::Animation;
    namespace T   = Desert::Animation::Timeline;
    namespace ECS = Desert::ECS;
    using Common::Content::AssetGuid;

    // A sampled parameter that must exist: an absent one fails the test here and reads as zero after.
    template <typename V>
    V Engaged( const std::optional<V>& value )
    {
        EXPECT_TRUE( value.has_value() ) << "the track has no value at that frame";
        return value.has_value() ? *value : V{};
    }

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

namespace
{
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
} // namespace

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

TEST( LevelSequenceDocument, AnEventKeyFiresOnTheStepThatCrossesItsTickAndOnlyThere )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  bound    = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( bound.IsSuccess() );
    const auto door   = bound.GetValue();
    const auto master = ECS::LevelSequenceMasterBinding();
    ASSERT_TRUE( ECS::AddEventTrack( sequence, master ).IsSuccess() );
    ASSERT_TRUE( ECS::AddEventTrack( sequence, door ).IsSuccess() ) << "an actor carries an Event track too";
    EXPECT_FALSE( ECS::AddEventTrack( sequence, door ).IsSuccess() ) << "one Event track per binding";
    EXPECT_FALSE( ECS::AddEventKey( sequence, door, A::FrameNumber{ 10 }, "" ).IsSuccess() )
         << "a name is required";

    ASSERT_TRUE( ECS::AddEventKey( sequence, master, A::FrameNumber{ 30 }, "Open" ).IsSuccess() );
    const auto knock = ECS::AddEventKey( sequence, door, A::FrameNumber{ 60 }, "Knock" );
    ASSERT_TRUE( knock.IsSuccess() ) << knock.GetError();
    ASSERT_TRUE( ECS::RenameEventKey( sequence, door, knock.GetValue(), "Slam" ).IsSuccess() );
    // A key moves and deletes like any other key: one more on the door, moved past "Slam", then removed.
    const auto temp = ECS::AddEventKey( sequence, door, A::FrameNumber{ 10 }, "Temp" );
    ASSERT_TRUE( temp.IsSuccess() );
    EXPECT_EQ( temp.GetValue(), 0U );
    const auto moved = ECS::MoveEventKey( sequence, door, temp.GetValue(), 80 );
    ASSERT_TRUE( moved.IsSuccess() ) << moved.GetError();
    EXPECT_EQ( moved.GetValue(), 1U ) << "tick 90 sorts after Slam at 60";
    EXPECT_FALSE( ECS::MoveEventKey( sequence, door, moved.GetValue(), 100000 ).IsSuccess() );
    ASSERT_TRUE( ECS::RemoveEventKey( sequence, door, moved.GetValue() ).IsSuccess() );
    ASSERT_TRUE( T::Validate( sequence ).IsSuccess() ) << T::Validate( sequence ).GetError();

    const auto doorKeys = ECS::EventKeys( sequence, door );
    ASSERT_EQ( doorKeys.size(), 1U );
    EXPECT_EQ( doorKeys[0].Tick.Value, 60 );
    EXPECT_EQ( doorKeys[0].Name, "Slam" );

    const auto text = Desert::Assets::LevelSequenceAsset::Write( sequence, AssetGuid{ 1, 2 } );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( parsed.GetValue().Sequence );
    const auto                        fired = [&]( int32_t from, int32_t to )
    {
        return ECS::StepLevelSequence( world.registry, component, playback, T::TimeStep{ At( from ), At( to ) } )
             .FiredEvents;
    };
    EXPECT_TRUE( fired( 0, 20 ).empty() );
    EXPECT_EQ( fired( 20, 40 ), std::vector<std::string>{ "Open" } );
    EXPECT_TRUE( fired( 40, 50 ).empty() );
    EXPECT_EQ( fired( 50, 70 ), std::vector<std::string>{ "Slam" } );
    EXPECT_TRUE( fired( 70, 100 ).empty() );
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
    T::Sequence               sequence = AuthoredDoor();
    const auto                door     = sequence.Bindings.front().Guid;
    World                     world;
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
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ) ).x,
                     0.75F );
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 20 } ) ).x,
                     0.5F )
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
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, albedo, A::FrameNumber{ 70 } ) ).z, 0.3F );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, albedo ).size(), 1U );

    // Ctrl+Z ×3: the vector track goes, then only the key (the start key stays), then the scalar track.
    ASSERT_TRUE( history.Undo() );
    EXPECT_FALSE( ECS::HasMaterialParameterTrack( sequence, door, albedo ) );
    ASSERT_TRUE( history.Undo() );
    ASSERT_TRUE( ECS::HasMaterialParameterTrack( sequence, door, roughness ) );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, roughness ).size(), 1U );
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ) ).x,
                     0.25F );
    ASSERT_TRUE( history.Undo() );
    EXPECT_FALSE( ECS::HasMaterialParameterTrack( sequence, door, roughness ) );
    EXPECT_FALSE( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ).has_value() );
    EXPECT_EQ( sequence.Tracks.size(), tracksBefore );
    EXPECT_FALSE( history.Undo() ) << "nothing else was recorded";

    // Ctrl+Y ×2: the track, then its key, each by value.
    ASSERT_TRUE( history.Redo() );
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ) ).x,
                     0.25F );
    ASSERT_TRUE( history.Redo() );
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, roughness, A::FrameNumber{ 40 } ) ).x,
                     0.75F );
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
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, blend, playhead ) ).x, 1.0F );
    EXPECT_FLOAT_EQ( LM::Describe( sequence, playhead, schema )[0].Value[0], 1.0F )
         << "the census reads the keyed value back at the playhead";

    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( ECS::MaterialParameterKeyTicks( sequence, door, blend ).size(), 1U ) << "Ctrl+Z takes the key back";
    EXPECT_FLOAT_EQ( Engaged( ECS::MaterialParameterAt( sequence, door, blend, playhead ) ).x, 0.0F );
    EXPECT_FALSE( history.Undo() ) << "nothing else was recorded";
    history.Clear();
}

// ── SEQ1a: TRANSFORM KEYS THROUGH THE FILE, AND THE PLAYER STEP THE SYSTEM RUNS ─────────────────────────────
namespace
{
    /// The Translation channel of @p sequence's first Transform track (the door's, in AuthoredDoor).
    T::FloatChannel* DoorTranslationX( T::Sequence& sequence )
    {
        for ( auto& track : sequence.Tracks )
        {
            if ( track.Property != ECS::kLevelSequenceTransformProperty || track.Sections.empty() )
                continue;
            auto* channel = std::get_if<T::Channel>( &track.Sections.front().Content );
            auto* pose    = channel != nullptr ? std::get_if<T::TransformChannel>( channel ) : nullptr;
            if ( pose != nullptr )
                return &pose->Translation.X;
        }
        return nullptr;
    }

    float DoorX( const World& world )
    {
        return world.registry.get<ECS::TransformComponent>( world.door ).Translation.x;
    }

    /// Seconds of game time that advance @p ticks of AuthoredDoor's tick rate at play rate 1.
    double SecondsOf( const T::Sequence& sequence, int32_t ticks )
    {
        return static_cast<double>( ticks ) * sequence.TickRate.Denominator / sequence.TickRate.Numerator;
    }
} // namespace

TEST( LevelSequenceAsset, ATransformTrackKeepsEveryInterpolationAndTangentThroughTheFile )
{
    T::Sequence             authored = AuthoredDoor();
    auto                    door     = authored.Bindings.front().Guid;
    ECS::TransformComponent middle;
    middle.Translation.x = 30.0F;
    ASSERT_TRUE( ECS::SetEntityTransformKey( authored, door, A::FrameNumber{ 50 }, ECS::EntityPose( middle ) )
                      .IsSuccess() );
    T::FloatChannel* x = DoorTranslationX( authored );
    ASSERT_NE( x, nullptr );
    ASSERT_EQ( x->Keys.size(), 3U );
    x->Keys[0].Interp        = A::KeyInterp::Constant;
    x->Keys[1].Interp        = A::KeyInterp::Cubic;
    x->Keys[1].Mode          = A::TangentMode::User;
    x->Keys[1].ArriveTangent = -55.5F;
    x->Keys[1].LeaveTangent  = 1234.25F;
    x->Keys[2].Interp        = A::KeyInterp::Linear;
    ASSERT_TRUE( T::Validate( authored ).IsSuccess() ) << T::Validate( authored ).GetError();

    const AssetGuid guid{ 0xA, 0xB };
    const auto      text = Desert::Assets::LevelSequenceAsset::Write( authored, guid );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Guid, guid );

    T::Sequence      read  = parsed.GetValue().Sequence;
    T::FloatChannel* readX = DoorTranslationX( read );
    ASSERT_NE( readX, nullptr );
    ASSERT_EQ( readX->Keys.size(), x->Keys.size() );
    for ( size_t i = 0; i < x->Keys.size(); ++i )
    {
        EXPECT_EQ( readX->Keys[i].Tick, x->Keys[i].Tick ) << "key " << i;
        EXPECT_EQ( readX->Keys[i].Value, x->Keys[i].Value ) << "key " << i;
        EXPECT_EQ( readX->Keys[i].Interp, x->Keys[i].Interp ) << "key " << i;
        EXPECT_EQ( readX->Keys[i].Mode, x->Keys[i].Mode ) << "key " << i;
        EXPECT_EQ( readX->Keys[i].ArriveTangent, x->Keys[i].ArriveTangent ) << "key " << i;
        EXPECT_EQ( readX->Keys[i].LeaveTangent, x->Keys[i].LeaveTangent ) << "key " << i;
    }
    for ( const int32_t tick : { 0, 25, 49, 50, 60, 75, 100 } )
        EXPECT_EQ( T::Evaluate( *readX, At( tick ), read.TickRate ),
                   T::Evaluate( *x, At( tick ), authored.TickRate ) )
             << "tick " << tick;

    const auto again = Desert::Assets::LevelSequenceAsset::Write( read, guid );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_EQ( again.GetValue(), text.GetValue() );
}

TEST( LevelSequencePlayer, AdvancesAtThePlayRateAndLoopsTheBoundEntity )
{
    const T::Sequence           sequence = AuthoredDoor(); // door X = tick on 0..100
    World                       world;
    ECS::LevelSequenceComponent component;
    component.Loop     = T::LoopMode::Loop;
    component.PlayRate = 2.0;
    ECS::LevelSequencePlayback playback( sequence );
    playback.Player.Play();

    // 25 ticks of game time at rate 2 = 50 ticks of sequence.
    auto step = ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 25 ) );
    EXPECT_TRUE( step.Report.Unresolved.empty() );
    EXPECT_NEAR( playback.Player.Current().AsTicks(), 50.0, 1e-3 );
    EXPECT_NEAR( DoorX( world ), 50.0F, 1e-2F );

    // 30 more = 60 ticks: past the end (100), so the loop wraps and the door is back near the start.
    step = ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 30 ) );
    const double wrapped = playback.Player.Current().AsTicks();
    EXPECT_LT( wrapped, 50.0 ) << "Loop wraps 110 back into the range";
    EXPECT_EQ( playback.Player.State(), T::PlayState::Playing );
    EXPECT_NEAR( DoorX( world ), static_cast<float>( wrapped ), 1e-2F );

    // A rate changed while playing is the next step's rate: 0 holds the frame.
    component.PlayRate = 0.0;
    step = ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 30 ) );
    EXPECT_EQ( playback.Player.Current().AsTicks(), wrapped );
}

TEST( LevelSequencePlayer, OnceClampsTheBoundEntityOnTheLastKeyAndStops )
{
    const T::Sequence           sequence = AuthoredDoor();
    World                       world;
    ECS::LevelSequenceComponent component;
    component.Loop     = T::LoopMode::Once;
    component.PlayRate = 2.0;
    ECS::LevelSequencePlayback playback( sequence );
    playback.Player.Play();
    (void)ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 25 ) );
    (void)ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 30 ) );
    EXPECT_NEAR( DoorX( world ), 100.0F, 1e-3F );
    EXPECT_NE( playback.Player.State(), T::PlayState::Playing );
}

TEST( LevelSequencePlayer, ABindingToAMissingEntityIsReportedAndThePlayerGoesOn )
{
    T::Sequence sequence              = AuthoredDoor();
    sequence.Bindings.front().Locator = "987654321"; // no entity of the scene carries this UUID
    World                       world;
    ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback  playback( sequence );
    playback.Player.Play();
    ECS::LevelSequenceActorState state;
    const auto step = ECS::AdvanceLevelSequence( world.registry, component, playback, SecondsOf( sequence, 50 ) );
    EXPECT_EQ( step.Report.Unresolved.size(), 1U );
    const auto errors = ECS::TakeNewLevelSequenceErrors( state, step );
    ASSERT_EQ( errors.size(), 1U );
    EXPECT_NE( errors.front().find( "'Door'" ), std::string::npos ) << errors.front();
    EXPECT_NEAR( playback.Player.Current().AsTicks(), 50.0, 1e-3 ) << "the transport still advanced";
    EXPECT_EQ( DoorX( world ), 0.0F );
}

// ── SEQ1b: event actions and the Subsequence track in the .dseq ──────────────────────────────────────

namespace
{
    /// DoorAndCut plus a sequence-level Event track whose keys carry each action kind, and a Subsequence track.
    T::Sequence WithActionsAndSubsequence()
    {
        T::Sequence    sequence = DoorAndCut( std::to_string( kDoorUuid ) );
        T::BindingGuid master;
        for ( const T::Binding& binding : sequence.Bindings )
            if ( binding.Kind == T::BindingKind::Sequence )
                master = binding.Guid;

        T::Track    events{ master, "Actions", T::TrackKind::Event, {}, false };
        T::Section& eventSection = T::AddSection( events, T::FrameNumber{ 0 }, T::FrameNumber{ 48000 } );
        auto&       keys         = std::get<T::EventChannel>( std::get<T::Channel>( eventSection.Content ) ).Keys;
        keys.push_back( T::EventKey{ T::FrameNumber{ 100 }, T::FrameNumber{ 0 }, "Bell", 0,
                                     T::EventAction{ T::EventActionKind::PlaySound, "Audio/Bell.wav" } } );
        keys.push_back( T::EventKey{ T::FrameNumber{ 200 }, T::FrameNumber{ 0 }, "Sparks", 0,
                                     T::EventAction{ T::EventActionKind::ActivateParticles, "" } } );
        keys.push_back( T::EventKey{ T::FrameNumber{ 300 }, T::FrameNumber{ 0 }, "Open", 0,
                                     T::EventAction{ T::EventActionKind::CallScript, "OnDoorCue" } } );
        keys.push_back( T::EventKey{ T::FrameNumber{ 400 }, T::FrameNumber{ 0 }, "Marker", 0, std::nullopt } );
        sequence.Tracks.push_back( std::move( events ) );

        T::Track    sub{ master, "Subsequence", T::TrackKind::Subsequence, {}, false };
        T::Section& subSection = T::AddSection( sub, T::FrameNumber{ 24000 }, T::FrameNumber{ 72000 } );
        subSection.Content =
             T::SubsequenceSectionContent{ AssetGuid{ 0xABCD, 0xEF01 }, T::FrameNumber{ 600 }, 0.5 };
        sequence.Tracks.push_back( std::move( sub ) );
        return sequence;
    }
} // namespace

TEST( LevelSequenceAsset, EventActionsAndASubsequenceKeepEveryFieldThroughTheFile )
{
    const T::Sequence sequence = WithActionsAndSubsequence();
    ASSERT_TRUE( T::Validate( sequence ).IsSuccess() ) << T::Validate( sequence ).GetError();
    const AssetGuid guid{ 0x1234, 0x5678 };

    const auto text = Desert::Assets::LevelSequenceAsset::Write( sequence, guid );
    ASSERT_TRUE( text.IsSuccess() ) << text.GetError();
    const auto parsed = Desert::Assets::LevelSequenceAsset::Parse( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const T::Sequence& back = parsed.GetValue().Sequence;
    ASSERT_EQ( back.Tracks.size(), sequence.Tracks.size() );

    const auto& keys = std::get<T::EventChannel>(
                            std::get<T::Channel>( back.Tracks[back.Tracks.size() - 2].Sections[0].Content ) )
                            .Keys;
    ASSERT_EQ( keys.size(), 4U );
    ASSERT_TRUE( keys[0].Action && keys[1].Action && keys[2].Action );
    EXPECT_EQ( keys[0].Action->Kind, T::EventActionKind::PlaySound );
    EXPECT_EQ( keys[0].Action->Target, "Audio/Bell.wav" );
    EXPECT_EQ( keys[1].Action->Kind, T::EventActionKind::ActivateParticles );
    EXPECT_EQ( keys[2].Action->Kind, T::EventActionKind::CallScript );
    EXPECT_EQ( keys[2].Action->Target, "OnDoorCue" );
    EXPECT_FALSE( keys[3].Action ) << "a plain marker stays a marker";

    const T::Track& sub = back.Tracks.back();
    EXPECT_EQ( sub.Kind, T::TrackKind::Subsequence );
    const auto& content = std::get<T::SubsequenceSectionContent>( sub.Sections[0].Content );
    EXPECT_EQ( content.Sequence, ( AssetGuid{ 0xABCD, 0xEF01 } ) );
    EXPECT_EQ( content.StartOffset.Value, 600 );
    EXPECT_EQ( content.TimeScale, 0.5 );
    EXPECT_NE( text.GetValue().find( "abcd" ), std::string::npos )
         << "the played sequence is a header dependency, as a played clip is";

    const auto again = Desert::Assets::LevelSequenceAsset::Write( back, guid );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_EQ( again.GetValue(), text.GetValue() );
}

TEST( LevelSequenceAsset, AnActionWithoutItsTargetAndAStoppedSubsequenceAreRefused )
{
    T::Sequence noTarget = WithActionsAndSubsequence();
    std::get<T::EventChannel>(
         std::get<T::Channel>( noTarget.Tracks[noTarget.Tracks.size() - 2].Sections[0].Content ) )
         .Keys[0]
         .Action->Target.clear();
    EXPECT_FALSE( T::Validate( noTarget ).IsSuccess() );

    T::Sequence stopped = WithActionsAndSubsequence();
    std::get<T::SubsequenceSectionContent>( stopped.Tracks.back().Sections[0].Content ).TimeScale = 0.0;
    const auto refused = T::Validate( stopped );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "time scale" ), std::string::npos ) << refused.GetError();
}

// ── SEQ1b: event actions, subsequences ─────────────────────────────────────────────────────────────────

namespace
{
    T::EventKey EventAt( int32_t tick, const std::string& name,
                         std::optional<T::EventAction> action = std::nullopt )
    {
        T::EventKey key;
        key.Tick   = A::FrameNumber{ tick };
        key.Name   = name;
        key.Action = std::move( action );
        return key;
    }

    /// "Door" (entity kDoorUuid) carries one Event track over ticks 0..100 holding @p keys.
    T::Sequence EventDoor( std::vector<T::EventKey> keys )
    {
        T::Sequence sequence;
        sequence.Host  = T::SequenceHost::LevelSequence;
        sequence.Start = A::FrameNumber{ 0 };
        sequence.End   = A::FrameNumber{ 100 };
        sequence.Bindings.push_back(
             T::Binding{ Guid( 1 ), T::BindingKind::Entity, std::to_string( kDoorUuid ), "Door", {} } );
        T::Track events;
        events.Binding = Guid( 1 );
        events.Kind    = T::TrackKind::Event;
        T::Section section;
        section.Start = A::FrameNumber{ 0 };
        section.End   = A::FrameNumber{ 100 };
        T::EventChannel channel;
        channel.Keys    = std::move( keys );
        section.Content = T::Channel{ channel };
        events.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( events ) );
        return sequence;
    }

    /// A sequence whose master binding plays @p sub over parent ticks [start, end].
    T::Sequence Playing( const AssetGuid& sub, int32_t start, int32_t end, int32_t offset = 0, double scale = 1.0 )
    {
        T::Sequence sequence;
        sequence.Host  = T::SequenceHost::LevelSequence;
        sequence.Start = A::FrameNumber{ 0 };
        sequence.End   = A::FrameNumber{ 100 };
        sequence.Bindings.push_back( T::Binding{ Guid( 3 ), T::BindingKind::Sequence, "", "Master", {} } );
        T::Track track;
        track.Binding = Guid( 3 );
        track.Kind    = T::TrackKind::Subsequence;
        T::Section section;
        section.Start   = A::FrameNumber{ start };
        section.End     = A::FrameNumber{ end };
        section.Content = T::SubsequenceSectionContent{ sub, A::FrameNumber{ offset }, scale };
        track.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( track ) );
        return sequence;
    }

    ECS::LevelSequenceSubsequenceSource SourceOf( const std::map<std::string, const T::Sequence*>& byName,
                                                  const std::map<std::string, AssetGuid>&          guids )
    {
        ECS::LevelSequenceSubsequenceSource source;
        const auto                          nameOf = [guids]( const AssetGuid& guid )
        {
            for ( const auto& [name, g] : guids )
                if ( g == guid )
                    return name;
            return std::string( "?" );
        };
        source.Find = [byName, nameOf]( const AssetGuid& guid ) -> const T::Sequence*
        {
            const auto found = byName.find( nameOf( guid ) );
            return found != byName.end() ? found->second : nullptr;
        };
        source.Name = nameOf;
        return source;
    }
} // namespace

TEST( LevelSequencePlayback, AMaterialScalarInterpolatesBetweenItsKeysAtEveryTick )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  added    = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( added.IsSuccess() );
    const ECS::LevelSequenceMaterialParameter glow{ 0, "Emissive" };
    ASSERT_TRUE(
         ECS::AddMaterialParameterTrack( sequence, added.GetValue(), glow, T::TrackKind::Float, glm::vec4( 0.0F ) )
              .IsSuccess() );
    ASSERT_TRUE(
         ECS::SetMaterialParameterKey( sequence, added.GetValue(), glow, A::FrameNumber{ 20 }, glm::vec4( 2.0F ) )
              .IsSuccess() );
    ASSERT_TRUE(
         ECS::SetMaterialParameterKey( sequence, added.GetValue(), glow, A::FrameNumber{ 60 }, glm::vec4( 6.0F ) )
              .IsSuccess() );

    World             world;
    FakeMaterialSlots slots;
    slots.Owner = world.door;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );
    for ( const auto [tick, expected] :
          { std::pair{ 20, 2.0F }, std::pair{ 30, 3.0F }, std::pair{ 50, 5.0F }, std::pair{ 60, 6.0F } } )
    {
        const auto step =
             ECS::StepLevelSequence( world.registry, component, playback, Step( tick ), {}, slots.Access() );
        EXPECT_TRUE( step.Refusals.empty() );
        EXPECT_FLOAT_EQ( slots.Overrides.at( { world.door, 0U, std::string( "Emissive" ) } ).x, expected )
             << "linear keys 2 @20 and 6 @60, sampled at " << tick;
    }
}

TEST( LevelSequencePlayback, AnEventFiresOncePerForwardCrossingNoneOnABackwardScrubAndASkipFiresAllInOrder )
{
    World             world;
    const T::Sequence sequence = EventDoor( { EventAt( 20, "A" ), EventAt( 40, "B" ), EventAt( 60, "C" ) } );
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );
    const auto                        fired = [&]( const T::TimeStep& step )
    { return ECS::StepLevelSequence( world.registry, component, playback, step ).FiredEvents; };

    EXPECT_EQ( fired( T::TimeStep{ At( 10 ), At( 20 ) } ), std::vector<std::string>{ "A" } );
    EXPECT_TRUE( fired( T::TimeStep{ At( 20 ), At( 30 ) } ).empty() )
         << "A was crossed on the step that reached it";
    EXPECT_TRUE( fired( Step( 5 ) ).empty() ) << "a scrub back (a jump) crosses nothing";
    EXPECT_EQ( fired( T::TimeStep{ At( 5 ), At( 95 ) } ), ( std::vector<std::string>{ "A", "B", "C" } ) )
         << "one large step fires every skipped key, in tick order";
}

TEST( LevelSequencePlayback, EventActionsPlayTheSoundRestartTheEmitterAndQueueTheScriptCall )
{
    World             world;
    const T::Sequence sequence = EventDoor(
         { EventAt( 10, "Boom", T::EventAction{ T::EventActionKind::PlaySound, "Assets/Audio/boom.wav" } ),
           EventAt( 20, "Sparks", T::EventAction{ T::EventActionKind::ActivateParticles, "" } ),
           EventAt( 30, "Open", T::EventAction{ T::EventActionKind::CallScript, "OnDoorOpen" } ) } );
    world.registry.emplace<ECS::ParticleEmitterComponent>( world.door ).Data.Enabled = false;
    world.registry.emplace<ECS::ScriptComponent>( world.door );
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( sequence );

    const auto step =
         ECS::StepLevelSequence( world.registry, component, playback, T::TimeStep{ At( 0 ), At( 35 ) } );
    EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
    EXPECT_EQ( step.Sounds, std::vector<std::string>{ "Assets/Audio/boom.wav" } );
    const auto& emitter = world.registry.get<ECS::ParticleEmitterComponent>( world.door );
    EXPECT_TRUE( emitter.Data.Enabled );
    EXPECT_TRUE( emitter.RequestRestart );
    const auto& calls = world.registry.get<ECS::ScriptComponent>( world.door ).PendingSequenceCalls;
    ASSERT_EQ( calls.size(), 1U );
    EXPECT_EQ( calls[0].Function, "OnDoorOpen" );
    EXPECT_EQ( calls[0].EventName, "Open" );

    // No emitter, no script: each refused by its key's name, never skipped in silence.
    world.registry.remove<ECS::ParticleEmitterComponent>( world.door );
    world.registry.remove<ECS::ScriptComponent>( world.door );
    ECS::LevelSequencePlayback bare( sequence );
    const auto                 refused =
         ECS::StepLevelSequence( world.registry, component, bare, T::TimeStep{ At( 0 ), At( 35 ) } );
    ASSERT_EQ( refused.Refusals.size(), 2U );
    EXPECT_NE( refused.Refusals[0].find( "Sparks" ), std::string::npos ) << refused.Refusals[0];
    EXPECT_NE( refused.Refusals[1].find( "Open" ), std::string::npos ) << refused.Refusals[1];
}

TEST( LevelSequencePlayback, ASubsequenceTimeIsTheOffsetPlusTheScaledParentTimeInItsOwnTicks )
{
    T::Section section;
    section.Start = A::FrameNumber{ 20 };
    section.End   = A::FrameNumber{ 80 };
    const T::SubsequenceSectionContent content{ AssetGuid{ 9, 1 }, A::FrameNumber{ 5 }, 2.0 };
    EXPECT_DOUBLE_EQ(
         T::MapSubsequenceTime( section, content, A::FrameRate{ 60, 1 }, A::FrameRate{ 60, 1 }, At( 30 ) )
              .AsTicks(),
         25.0 )
         << "5 + (30 - 20) x 2";
    EXPECT_DOUBLE_EQ(
         T::MapSubsequenceTime( section, content, A::FrameRate{ 60, 1 }, A::FrameRate{ 30, 1 }, At( 30 ) )
              .AsTicks(),
         15.0 )
         << "a 30-tick subsequence advances half as many ticks";
    EXPECT_DOUBLE_EQ(
         T::MapSubsequenceTime( section, content, A::FrameRate{ 60, 1 }, A::FrameRate{ 60, 1 }, At( 20 ) )
              .AsTicks(),
         5.0 )
         << "the section's first tick is the offset";
}

TEST( LevelSequencePlayback, ASubsequenceMovesItsActorsAndFiresItsEventsAtTheMappedTime )
{
    // Inner (30 ticks/s): Door X 0 -> 100 over its ticks 0..100, an event at its tick 10.
    T::Sequence inner = DoorAndCut( std::to_string( kDoorUuid ) );
    inner.TickRate    = A::FrameRate{ 30, 1 };
    inner.Tracks.pop_back(); // no camera cut
    T::Track events;
    events.Binding = Guid( 3 );
    events.Kind    = T::TrackKind::Event;
    T::Section keys;
    keys.Start = A::FrameNumber{ 0 };
    keys.End   = A::FrameNumber{ 100 };
    T::EventChannel channel;
    channel.Keys = { EventAt( 10, "Inner" ) };
    keys.Content = T::Channel{ channel };
    events.Sections.push_back( std::move( keys ) );
    inner.Tracks.push_back( std::move( events ) );

    const AssetGuid   innerGuid{ 9, 1 };
    const T::Sequence outer  = Playing( innerGuid, 20, 80 ); // 60 ticks/s: inner tick = (parent - 20) / 2
    const auto        source = SourceOf( { { "Inner", &inner } }, { { "Inner", innerGuid } } );

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( outer );
    const auto                        step = [&]( int32_t from, int32_t to )
    {
        return ECS::StepLevelSequence( world.registry, component, playback, T::TimeStep{ At( from ), At( to ) },
                                       {}, {}, source );
    };
    EXPECT_TRUE( step( 0, 30 ).FiredEvents.empty() );
    const auto crossing = step( 30, 60 );
    EXPECT_TRUE( crossing.Refusals.empty() ) << crossing.Refusals.front();
    EXPECT_EQ( crossing.FiredEvents, std::vector<std::string>{ "Inner" } ) << "inner tick 10 = parent tick 40";
    EXPECT_FLOAT_EQ( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 20.0F )
         << "parent tick 60 = inner tick 20";
    EXPECT_TRUE( step( 60, 100 ).FiredEvents.empty() ) << "crossed once";

    // Without the subsequence's sequence the section is refused by its name.
    ECS::LevelSequencePlayback blind( outer );
    const auto refused = ECS::StepLevelSequence( world.registry, component, blind, Step( 50 ), {}, {},
                                                 SourceOf( {}, { { "Inner", innerGuid } } ) );
    ASSERT_EQ( refused.Refusals.size(), 1U );
    EXPECT_NE( refused.Refusals[0].find( "Inner" ), std::string::npos ) << refused.Refusals[0];
}

TEST( LevelSequencePlayback, ASubsequenceCycleIsRefusedByNameBeforeAndDuringPlay )
{
    const AssetGuid   a{ 9, 1 };
    const AssetGuid   b{ 9, 2 };
    const AssetGuid   c{ 9, 3 };
    const T::Sequence seqA = Playing( b, 0, 100 );
    const T::Sequence seqB = Playing( a, 0, 100 );
    const T::Sequence seqC = Playing( c, 0, 100 );
    const auto        source =
         SourceOf( { { "A", &seqA }, { "B", &seqB }, { "C", &seqC } }, { { "A", a }, { "B", b }, { "C", c } } );

    const auto ab = ECS::CheckSubsequenceCycles( a, source );
    ASSERT_FALSE( ab.IsSuccess() );
    EXPECT_EQ( ab.GetError(), "subsequence cycle: A -> B -> A" );
    const auto self = ECS::CheckSubsequenceCycles( c, source );
    ASSERT_FALSE( self.IsSuccess() );
    EXPECT_EQ( self.GetError(), "subsequence cycle: C -> C" );
    EXPECT_TRUE(
         ECS::CheckSubsequenceCycles( a, SourceOf( { { "A", &seqA } }, { { "A", a }, { "B", b } } ) ).IsSuccess() )
         << "B not loaded: nothing to follow, no cycle";

    World                             world;
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback        playback( seqA );
    playback.Asset    = a;
    const auto played = ECS::StepLevelSequence( world.registry, component, playback, Step( 50 ), {}, {}, source );
    ASSERT_EQ( played.Refusals.size(), 1U );
    EXPECT_EQ( played.Refusals[0], "subsequence cycle: A -> B -> A" );
}

// ── SEQ1c: the Sequencer's command layer for key shape, easing, event actions, subsequence sections ─────────
namespace
{
    namespace Ed1c = Desert::Editor;

    /// The Transform channel of @p sequence's first Transform track's first section.
    const T::TransformChannel& DoorPose( const T::Sequence& sequence )
    {
        for ( const T::Track& track : sequence.Tracks )
            if ( track.Property == ECS::kLevelSequenceTransformProperty )
                return std::get<T::TransformChannel>( std::get<T::Channel>( track.Sections.front().Content ) );
        throw std::runtime_error( "no Transform track" );
    }

    const A::ScalarKey& KeyOn( const T::FloatChannel& lane, int32_t tick )
    {
        for ( const A::ScalarKey& key : lane.Keys )
            if ( key.Tick.Value == tick )
                return key;
        throw std::runtime_error( "no key on that tick" );
    }

    Ed1c::SequenceOwner OwnerOf( T::Sequence& sequence )
    {
        Ed1c::SequenceOwner owner;
        owner.Identity = &sequence;
        owner.Resolve  = [&sequence]() -> T::Sequence* { return &sequence; };
        owner.Volatile = false;
        owner.Name     = "Level Sequence";
        return owner;
    }
} // namespace

TEST( LevelSequenceKeys, KeyShapeSetsEveryLaneOfTheKeyAndRotationStaysASlerp )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  door     = sequence.Bindings.front().Guid;

    ASSERT_TRUE( ECS::SetEntityTransformKeyShape( sequence, door, { A::FrameNumber{ 100 } }, A::KeyInterp::Cubic,
                                                  A::TangentMode::User )
                      .IsSuccess() );
    const T::TransformChannel& pose = DoorPose( sequence );
    EXPECT_EQ( KeyOn( pose.Translation.X, 100 ).Interp, A::KeyInterp::Cubic );
    EXPECT_EQ( KeyOn( pose.Scale.Z, 100 ).Interp, A::KeyInterp::Cubic );
    EXPECT_EQ( KeyOn( pose.Translation.Y, 100 ).Mode, A::TangentMode::User );
    EXPECT_EQ( KeyOn( pose.Rotation.W, 100 ).Interp, A::KeyInterp::Linear ) << "a quaternion lane is never Cubic";
    EXPECT_EQ( KeyOn( pose.Translation.X, 0 ).Interp, A::KeyInterp::Linear ) << "only the named key changed";

    // A tick with no key refuses the whole edit.
    const T::Sequence before = sequence;
    EXPECT_FALSE( ECS::SetEntityTransformKeyShape( sequence, door, { A::FrameNumber{ 0 }, A::FrameNumber{ 55 } },
                                                   A::KeyInterp::Constant, A::TangentMode::Auto )
                       .IsSuccess() );
    EXPECT_EQ( sequence.Revision, before.Revision );
    EXPECT_EQ( KeyOn( DoorPose( sequence ).Translation.X, 0 ).Interp, A::KeyInterp::Linear );
}

TEST( LevelSequenceKeys, AnEaseInOutOnLocationInsertsTheMiddleKeyAndRefusesRotationAndTheFirstKey )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  door     = sequence.Bindings.front().Guid;

    EXPECT_FALSE( ECS::ApplyEntityTransformEasing( sequence, door, A::TrackChannel::Rotation,
                                                   A::FrameNumber{ 100 }, T::EasingPreset::QuadInOut )
                       .IsSuccess() );
    EXPECT_FALSE( ECS::ApplyEntityTransformEasing( sequence, door, A::TrackChannel::Position, A::FrameNumber{ 0 },
                                                   T::EasingPreset::QuadInOut )
                       .IsSuccess() )
         << "no segment ends on the first key";
    ASSERT_TRUE( ECS::ApplyEntityTransformEasing( sequence, door, A::TrackChannel::Position, A::FrameNumber{ 100 },
                                                  T::EasingPreset::QuadInOut )
                      .IsSuccess() );
    const T::TransformChannel& pose = DoorPose( sequence );
    EXPECT_EQ( pose.Translation.X.Keys.size(), 3U ) << "an InOut is two cubics: one key at the middle";
    EXPECT_EQ( pose.Rotation.X.Keys.size(), 2U ) << "Rotation untouched";
}

TEST( LevelSequenceEvents, AnActionIsSetClearedAndRefusedWithoutItsTarget )
{
    T::Sequence sequence = AuthoredDoor();
    const auto  master   = ECS::LevelSequenceMasterBinding();
    ASSERT_TRUE( ECS::AddEventTrack( sequence, master ).IsSuccess() );
    ASSERT_TRUE( ECS::AddEventKey( sequence, master, A::FrameNumber{ 10 }, "Boom" ).IsSuccess() );

    const uint64_t revision = sequence.Revision;
    EXPECT_FALSE(
         ECS::SetEventKeyAction( sequence, master, 0, T::EventAction{ T::EventActionKind::PlaySound, "" } )
              .IsSuccess() );
    EXPECT_EQ( sequence.Revision, revision );

    ASSERT_TRUE( ECS::SetEventKeyAction( sequence, master, 0,
                                         T::EventAction{ T::EventActionKind::PlaySound, "Audio/boom.wav" } )
                      .IsSuccess() );
    const auto keys = ECS::EventKeys( sequence, master );
    ASSERT_TRUE( keys.front().Action.has_value() );
    EXPECT_EQ( keys.front().Action->Target, "Audio/boom.wav" );

    ASSERT_TRUE( ECS::SetEventKeyAction( sequence, master, 0, std::nullopt ).IsSuccess() );
    EXPECT_FALSE( ECS::EventKeys( sequence, master ).front().Action.has_value() );
    EXPECT_FALSE( ECS::SetEventKeyAction( sequence, master, 7, std::nullopt ).IsSuccess() );
}

TEST( LevelSequenceSubsequenceEdit, ASectionIsAddedStackedEditedAndRemovedAndNeverPlaysItself )
{
    T::Sequence     sequence = AuthoredDoor();
    const AssetGuid self{ 1, 1 };
    const AssetGuid sub{ 2, 2 };

    EXPECT_FALSE( ECS::AddSubsequenceSection( sequence, self, self, A::FrameNumber{ 0 }, A::FrameNumber{ 50 } )
                       .IsSuccess() );
    const auto first =
         ECS::AddSubsequenceSection( sequence, self, sub, A::FrameNumber{ 0 }, A::FrameNumber{ 50 } );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    const auto second =
         ECS::AddSubsequenceSection( sequence, self, sub, A::FrameNumber{ 40 }, A::FrameNumber{ 90 } );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    auto sections = ECS::SubsequenceSections( sequence );
    ASSERT_EQ( sections.size(), 2U );
    EXPECT_EQ( sections[0].Row, 0 );
    EXPECT_EQ( sections[1].Row, 1 ) << "an overlapping section stacks on the next row";
    EXPECT_EQ( sections[0].Content.TimeScale, 1.0 );

    // A stopped section is Validate's refusal, and the sequence is as it was.
    const uint64_t revision = sequence.Revision;
    EXPECT_FALSE( ECS::SetSubsequenceSection( sequence, self, 0, A::FrameNumber{ 0 }, A::FrameNumber{ 50 },
                                              T::SubsequenceSectionContent{ sub, A::FrameNumber{ 0 }, 0.0 } )
                       .IsSuccess() );
    EXPECT_EQ( sequence.Revision, revision );

    ASSERT_TRUE( ECS::SetSubsequenceSection( sequence, self, 0, A::FrameNumber{ 0 }, A::FrameNumber{ 30 },
                                             T::SubsequenceSectionContent{ sub, A::FrameNumber{ 12 }, 2.0 } )
                      .IsSuccess() );
    sections = ECS::SubsequenceSections( sequence );
    EXPECT_EQ( sections[0].End.Value, 30 );
    EXPECT_EQ( sections[0].Content.StartOffset.Value, 12 );
    EXPECT_EQ( sections[0].Content.TimeScale, 2.0 );

    ASSERT_TRUE( ECS::RemoveSubsequenceSection( sequence, 1 ).IsSuccess() );
    EXPECT_EQ( ECS::SubsequenceSections( sequence ).size(), 1U );
}

// Every Sequencer edit is one ScopedSequenceEdit: one Ctrl+Z takes back exactly that edit.
TEST( LevelSequenceSubsequenceEdit, EachEditIsOneUndoStep )
{
    auto& history = Desert::Editor::CommandHistory::Get();
    history.Clear();
    T::Sequence                   sequence = AuthoredDoor();
    const auto                    door     = sequence.Bindings.front().Guid;
    const Ed1c::SequenceOwner     owner    = OwnerOf( sequence );
    Ed1c::SequenceEditTransaction transaction;
    {
        const Ed1c::ScopedSequenceEdit step( transaction, owner );
        ASSERT_TRUE( ECS::AddSubsequenceSection( sequence, AssetGuid{ 1, 1 }, AssetGuid{ 2, 2 },
                                                 A::FrameNumber{ 0 }, A::FrameNumber{ 50 } )
                          .IsSuccess() );
    }
    {
        const Ed1c::ScopedSequenceEdit step( transaction, owner );
        ASSERT_TRUE( ECS::SetEntityTransformKeyShape( sequence, door, { A::FrameNumber{ 100 } },
                                                      A::KeyInterp::Constant, A::TangentMode::Auto )
                          .IsSuccess() );
    }
    ASSERT_EQ( history.UndoStack().size(), 2U );
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( KeyOn( DoorPose( sequence ).Translation.X, 100 ).Interp, A::KeyInterp::Linear );
    EXPECT_EQ( ECS::SubsequenceSections( sequence ).size(), 1U ) << "the undo took back only the key shape";
    ASSERT_TRUE( history.Undo() );
    EXPECT_TRUE( ECS::SubsequenceSections( sequence ).empty() );
    history.Clear();
}

// The Sequencer's preview plays a subsequence at the mapped time when it is given the source, and restores
// the subsequence's actors on close; without a source the section is refused by name.
TEST( LevelSequencePreview, ASubsequencePosesItsActorAtTheMappedTickAndIsRestored )
{
    T::Sequence inner = DoorAndCut( std::to_string( kDoorUuid ) );
    inner.TickRate    = A::FrameRate{ 30, 1 };
    inner.Tracks.pop_back(); // no camera cut
    const AssetGuid innerGuid{ 9, 1 };
    T::Sequence     outer = Playing( innerGuid, 20, 80 );
    outer.TickRate        = A::FrameRate{ 60, 1 };
    const auto source     = SourceOf( { { "Inner", &inner } }, { { "Inner", innerGuid } } );

    World       world;
    const float startX = world.registry.get<ECS::TransformComponent>( world.door ).Translation.x;

    ECS::LevelSequencePreview blind;
    const auto                refused = blind.Scrub( world.registry, outer, A::FrameNumber{ 50 } );
    ASSERT_FALSE( refused.Refusals.empty() );
    EXPECT_NE( refused.Refusals.front().find( "loaded" ), std::string::npos ) << refused.Refusals.front();
    blind.Restore( world.registry );

    ECS::LevelSequencePreview preview;
    const auto                step =
         preview.Scrub( world.registry, outer, A::FrameNumber{ 50 }, {}, {}, source, AssetGuid{ 7, 7 } );
    EXPECT_TRUE( step.Refusals.empty() ) << step.Refusals.front();
    // Parent tick 50 is 30 ticks (0.5 s) into the section: the inner's tick 15 at 30 ticks/s; X 0 -> 100 over
    // 0..100.
    EXPECT_NEAR( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, 15.0F, 1e-3F );

    preview.Restore( world.registry );
    EXPECT_FLOAT_EQ( world.registry.get<ECS::TransformComponent>( world.door ).Translation.x, startX )
         << "the subsequence's actor was recorded and given back";
}

// "+ Track > Subsequence" and the section's sequence picker both refuse a pick that closes a loop, by name.
TEST( LevelSequenceSubsequenceEdit, AddingOrRetargetingASectionThatClosesACycleIsRefusedByName )
{
    const AssetGuid   a{ 9, 1 };
    const AssetGuid   b{ 9, 2 };
    const AssetGuid   c{ 9, 3 };
    const T::Sequence seqB = Playing( a, 0, 100 );
    const T::Sequence seqC = AuthoredDoor();
    const auto source = SourceOf( { { "B", &seqB }, { "C", &seqC } }, { { "A", a }, { "B", b }, { "C", c } } );

    T::Sequence    sequence = AuthoredDoor();
    const uint64_t revision = sequence.Revision;
    const auto     looped =
         ECS::AddSubsequenceSection( sequence, a, b, A::FrameNumber{ 0 }, A::FrameNumber{ 50 }, source );
    ASSERT_FALSE( looped.IsSuccess() );
    EXPECT_NE( looped.GetError().find( "subsequence cycle: A -> B -> A" ), std::string::npos )
         << looped.GetError();
    EXPECT_EQ( sequence.Revision, revision );
    EXPECT_TRUE( ECS::SubsequenceSections( sequence ).empty() );

    ASSERT_TRUE( ECS::AddSubsequenceSection( sequence, a, c, A::FrameNumber{ 0 }, A::FrameNumber{ 50 }, source )
                      .IsSuccess() );
    const auto retarget =
         ECS::SetSubsequenceSection( sequence, a, 0, A::FrameNumber{ 0 }, A::FrameNumber{ 50 },
                                     T::SubsequenceSectionContent{ b, A::FrameNumber{ 0 }, 1.0 }, source );
    ASSERT_FALSE( retarget.IsSuccess() );
    EXPECT_NE( retarget.GetError().find( "A -> B -> A" ), std::string::npos ) << retarget.GetError();
    EXPECT_EQ( ECS::SubsequenceSections( sequence ).front().Content.Sequence, c );
}

// The ruler's playback range handles: the range moves, the keys stay, an inverted range is refused whole.
TEST( LevelSequenceRange, APlaybackRangeIsSetKeysStayAndAnEndBeforeTheStartIsRefused )
{
    T::Sequence  sequence = AuthoredDoor();
    const size_t tracks   = sequence.Tracks.size();
    ASSERT_TRUE( ECS::SetPlaybackRange( sequence, A::FrameNumber{ 10 }, A::FrameNumber{ 40 } ).IsSuccess() );
    EXPECT_EQ( sequence.Start.Value, 10 );
    EXPECT_EQ( sequence.End.Value, 40 );
    EXPECT_EQ( sequence.Tracks.size(), tracks );

    const uint64_t revision = sequence.Revision;
    EXPECT_FALSE( ECS::SetPlaybackRange( sequence, A::FrameNumber{ 50 }, A::FrameNumber{ 20 } ).IsSuccess() );
    EXPECT_EQ( sequence.Start.Value, 10 );
    EXPECT_EQ( sequence.End.Value, 40 );
    EXPECT_EQ( sequence.Revision, revision );
}

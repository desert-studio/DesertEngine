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

#include <Common/Json/Document.hpp>

#include <gtest/gtest.h>

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
        const entt::entity entity = registry.create();
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
    World                        world;
    const T::Sequence            sequence = DoorAndCut( std::to_string( kDoorUuid ) );
    const ECS::LevelSequenceComponent component;
    ECS::LevelSequencePlayback   playback( sequence );

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
    ASSERT_TRUE( during.CameraCut.has_value() );
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
    ASSERT_TRUE( name.has_value() );
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
        const auto                text = Common::Json::Root( stored ).Find( "Loop" )->AsString();
        ASSERT_TRUE( text.IsSuccess() );
        EXPECT_EQ( text.GetValue(), T::ToString( mode ) );
        EXPECT_EQ( T::LoopModeFromString( T::ToString( mode ) ), mode );
    }
    EXPECT_FALSE( T::LoopModeFromString( "Sometimes" ).has_value() );
    EXPECT_STREQ( T::ToString( static_cast<T::LoopMode>( 7 ) ), "Unknown" );
}

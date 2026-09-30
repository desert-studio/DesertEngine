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

#include "../ClipFixture.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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
    ASSERT_TRUE( resolved.has_value() );
    EXPECT_EQ( static_cast<entt::entity>( static_cast<uint32_t>( resolved->Handle ) ), world.door );
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
    auto& animation = world.registry.emplace<ECS::AnimationComponent>( world.door,
                                                                       std::make_unique<A::Animator>( skeleton ) );
    ASSERT_TRUE( animation.Playing );

    T::Sequence sequence;
    sequence.Host  = T::SequenceHost::LevelSequence;
    sequence.Start = A::FrameNumber{ 0 };
    sequence.End   = A::FrameNumber{ 100 };
    const auto door = ECS::AddEntityBinding( sequence, Common::UUID( kDoorUuid ), "Door" );
    ASSERT_TRUE( door.IsSuccess() );
    const auto added = ECS::AddAnimationSection( sequence, door.GetValue(), walkGuid, A::FrameNumber{ 0 },
                                                 A::FrameNumber{ 100 }, true );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();

    const ECS::LevelSequenceClipSource clips = [&]( const AssetGuid& guid ) -> const A::AnimationClip*
    { return guid == walkGuid ? &walk : nullptr; };

    ECS::LevelSequencePreview preview;
    const auto step = preview.Scrub( world.registry, sequence, A::FrameNumber{ 10 }, clips );
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
    const auto refused = blind.Scrub( world.registry, sequence, A::FrameNumber{ 10 } );
    ASSERT_EQ( refused.Refusals.size(), 1U );
    EXPECT_NE( refused.Refusals.front().find( "no such clip" ), std::string::npos ) << refused.Refusals.front();

    preview.Restore( world.registry );
    EXPECT_TRUE( animation.Playing ) << "closing the preview gives the Animation component its playback back";
}

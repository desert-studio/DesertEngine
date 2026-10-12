#pragma once

// The level the LevelSequence suites play on, shared by the engine suite (Engine/LevelSequence) and the editor's
// (Editor/LevelSequenceEdit, which keys and undoes through the editor's transactions): ONE spelling of the door,
// its neighbours and the authored door sequence. Included by relative path; header-only.

#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>

namespace LevelSequenceFixture
{
    // A sampled parameter that must exist: an absent one fails the test here and reads as zero after.
    template <typename V>
    V Engaged( const std::optional<V>& value )
    {
        EXPECT_TRUE( value.has_value() ) << "the track has no value at that frame";
        return value.has_value() ? *value : V{};
    }

    inline constexpr uint64_t kDoorUuid   = 4101;
    inline constexpr uint64_t kOtherUuid  = 4102;
    inline constexpr uint64_t kCameraUuid = 4103;

    inline entt::entity Spawn( entt::registry& registry, uint64_t uuid )
    {
        const entt::entity entity                                   = registry.create();
        registry.emplace<Desert::ECS::UUIDComponent>( entity ).UUID = Common::UUID( uuid );
        registry.emplace<Desert::ECS::TransformComponent>( entity );
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
            registry.emplace<Desert::ECS::CameraComponent>( camera );
            registry.emplace<Desert::ECS::CameraComponent>( player );
        }
    };

    /// What the Sequencer's document authors: "+ Track → Actor" on the door, Transform keys X 0 at tick 0 and
    /// X 100 at tick 100 — through the document's own edits, not a hand-built sequence.
    inline Desert::Animation::Timeline::Sequence AuthoredDoor()
    {
        namespace A   = Desert::Animation;
        namespace T   = Desert::Animation::Timeline;
        namespace ECS = Desert::ECS;
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
    /// The Transform channel of @p sequence's first Transform track's first section.
    inline const Desert::Animation::Timeline::TransformChannel&
    DoorPose( const Desert::Animation::Timeline::Sequence& sequence )
    {
        namespace T = Desert::Animation::Timeline;
        for ( const T::Track& track : sequence.Tracks )
            if ( track.Property == Desert::ECS::kLevelSequenceTransformProperty )
                return std::get<T::TransformChannel>( std::get<T::Channel>( track.Sections.front().Content ) );
        throw std::runtime_error( "no Transform track" );
    }

    /// The key of @p lane on @p tick; throws when the lane has none there.
    inline const Desert::Animation::ScalarKey& KeyOn( const Desert::Animation::Timeline::FloatChannel& lane,
                                                      int32_t                                          tick )
    {
        for ( const Desert::Animation::ScalarKey& key : lane.Keys )
            if ( key.Tick.Value == tick )
                return key;
        throw std::runtime_error( "no key on that tick" );
    }
} // namespace LevelSequenceFixture

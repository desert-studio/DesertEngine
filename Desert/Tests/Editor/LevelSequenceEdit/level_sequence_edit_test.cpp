// THE LEVEL SEQUENCE'S EDITOR SIDE: an auto-keyed gizmo release, a Material Parameter track and its keys each as
// ONE undo step through the editor's CommandHistory and SequenceEditTransaction (Editor/Core/Commands/
// SequenceEdit.hpp), and the control channel's Material Parameter properties (Editor/Panels/Sequencer/
// LevelMaterialProperties.hpp). The engine half — the `.dseq` text, playback, the document's edits — is
// Engine/LevelSequence; this suite links the editor sources, so it lives in the editor's runner.

#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/SequenceEdit.hpp>
#include <Editor/Panels/Sequencer/LevelMaterialProperties.hpp>

#include "../../Engine/LevelSequence/LevelSequenceFixture.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace A   = Desert::Animation;
    namespace T   = Desert::Animation::Timeline;
    namespace ECS = Desert::ECS;

    using LevelSequenceFixture::AuthoredDoor;
    using LevelSequenceFixture::Engaged;
    using LevelSequenceFixture::World;
} // namespace

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

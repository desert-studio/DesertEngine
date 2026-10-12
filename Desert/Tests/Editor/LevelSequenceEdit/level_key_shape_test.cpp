// SEQ1c: the Sequencer's key-shape row shows the SELECTED keys' interpolation and tangent mode as they are in the
// sequence (UE: the curve editor's key menu; "Multiple Values" when the keys differ), never the last pick. The
// row's combos read ECS::SelectedEntityTransformKeyShape every frame and a pick applies to every selected key
// through ECS::SetEntityTransformKeyShape, so the two together are what the user sees after a pick.

#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

#include <Common/Core/UUID.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace
{
    namespace A   = Desert::Animation;
    namespace T   = Desert::Animation::Timeline;
    namespace ECS = Desert::ECS;

    struct Door
    {
        T::Sequence    Sequence;
        T::BindingGuid Binding;
    };

    /// A door with pose keys on ticks 0, 50 and 100 (Linear / Auto, as a new key is authored).
    Door ThreeKeyDoor()
    {
        Door door;
        door.Sequence.Host  = T::SequenceHost::LevelSequence;
        door.Sequence.Start = A::FrameNumber{ 0 };
        door.Sequence.End   = A::FrameNumber{ 100 };
        const auto binding = ECS::AddEntityBinding( door.Sequence, Desert::Common::UUID( 0x5E01C0DEULL ), "Door" );
        EXPECT_TRUE( binding.IsSuccess() );
        door.Binding = binding.GetValue();
        ECS::TransformComponent pose;
        for ( const int32_t tick : { 0, 50, 100 } )
        {
            pose.Translation.x = static_cast<float>( tick );
            EXPECT_TRUE( ECS::SetEntityTransformKey( door.Sequence, door.Binding, A::FrameNumber{ tick },
                                                     ECS::EntityPose( pose ) )
                              .IsSuccess() );
        }
        return door;
    }

    std::vector<ECS::TransformKeyRef> Select( const Door& door, const std::vector<int32_t>& ticks )
    {
        std::vector<ECS::TransformKeyRef> keys;
        for ( const int32_t tick : ticks )
            keys.push_back( ECS::TransformKeyRef{ door.Binding, A::FrameNumber{ tick } } );
        return keys;
    }
} // namespace

TEST( LevelKeyShapeRow, TwoKeysWithDifferentInterpShowMultipleValues )
{
    Door door = ThreeKeyDoor();
    ASSERT_TRUE( ECS::SetEntityTransformKeyShape( door.Sequence, door.Binding, { A::FrameNumber{ 50 } },
                                                  A::KeyInterp::Constant, std::nullopt )
                      .IsSuccess() );

    const ECS::TransformKeyShape shape =
         ECS::SelectedEntityTransformKeyShape( door.Sequence, Select( door, { 0, 50 } ) );
    EXPECT_FALSE( shape.Interp.has_value() ) << "Linear and Constant selected together are mixed";
    ASSERT_TRUE( shape.Mode.has_value() ) << "both keys are still Auto";
    EXPECT_EQ( *shape.Mode, A::TangentMode::Auto );
}

TEST( LevelKeyShapeRow, KeysWithTheSameShapeShowThatShape )
{
    const Door door = ThreeKeyDoor();

    const ECS::TransformKeyShape shape =
         ECS::SelectedEntityTransformKeyShape( door.Sequence, Select( door, { 0, 50, 100 } ) );
    ASSERT_TRUE( shape.Interp.has_value() );
    EXPECT_EQ( *shape.Interp, A::KeyInterp::Linear );
    ASSERT_TRUE( shape.Mode.has_value() );
    EXPECT_EQ( *shape.Mode, A::TangentMode::Auto );
}

TEST( LevelKeyShapeRow, AfterAPickTheRowShowsTheAppliedShapeAndTheOtherPartStays )
{
    Door                        door     = ThreeKeyDoor();
    const auto                  selected = Select( door, { 0, 100 } );
    std::vector<A::FrameNumber> ticks;
    for ( const auto& key : selected )
        ticks.push_back( key.Tick );

    // Cubic: the Rotation lanes store the slerp (Linear), yet the row must read Cubic, not "Multiple Values".
    ASSERT_TRUE(
         ECS::SetEntityTransformKeyShape( door.Sequence, door.Binding, ticks, A::KeyInterp::Cubic, std::nullopt )
              .IsSuccess() );
    ECS::TransformKeyShape shape = ECS::SelectedEntityTransformKeyShape( door.Sequence, selected );
    ASSERT_TRUE( shape.Interp.has_value() );
    EXPECT_EQ( *shape.Interp, A::KeyInterp::Cubic );
    ASSERT_TRUE( shape.Mode.has_value() );
    EXPECT_EQ( *shape.Mode, A::TangentMode::Auto ) << "picking the interpolation keeps the tangent mode";

    ASSERT_TRUE(
         ECS::SetEntityTransformKeyShape( door.Sequence, door.Binding, ticks, std::nullopt, A::TangentMode::Break )
              .IsSuccess() );
    shape = ECS::SelectedEntityTransformKeyShape( door.Sequence, selected );
    ASSERT_TRUE( shape.Mode.has_value() );
    EXPECT_EQ( *shape.Mode, A::TangentMode::Break );
    ASSERT_TRUE( shape.Interp.has_value() );
    EXPECT_EQ( *shape.Interp, A::KeyInterp::Cubic ) << "picking the tangent mode keeps the interpolation";

    // The unselected key in between was not touched: adding it to the selection is mixed on both parts.
    shape = ECS::SelectedEntityTransformKeyShape( door.Sequence, Select( door, { 0, 50, 100 } ) );
    EXPECT_FALSE( shape.Interp.has_value() );
    EXPECT_FALSE( shape.Mode.has_value() );
}

TEST( LevelKeyShapeRow, ARefusedEmptyShapeLeavesTheSequenceAsItWas )
{
    Door              door   = ThreeKeyDoor();
    const T::Sequence before = door.Sequence;
    EXPECT_FALSE( ECS::SetEntityTransformKeyShape( door.Sequence, door.Binding, { A::FrameNumber{ 0 } },
                                                   std::nullopt, std::nullopt )
                       .IsSuccess() );
    EXPECT_EQ( door.Sequence.Revision, before.Revision );
}

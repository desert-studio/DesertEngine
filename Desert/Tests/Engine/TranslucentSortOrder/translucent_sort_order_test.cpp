// The translucency pass's draw order, asserted as RELATIONS between objects rather than as positions in
// a list: the farther of two draws first; a higher priority draws after a lower one however near the
// lower one is; two objects on the same key keep their submission order (a stable sort — an unstable one
// flickers glass-behind-glass from frame to frame).
//
// Policy: Engine/Graphic/Systems/Scene/Mesh/TranslucentSortOrder.hpp (UE: FTranslucentPrimSet::
// SortPrimitives + UPrimitiveComponent::TranslucencySortPriority). Its one caller is
// MeshRenderer::RenderGlassManual.

#include <Engine/Graphic/Systems/Scene/Mesh/TranslucentSortOrder.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{
    using Desert::Graphic::System::TranslucentSortItem;
    using Desert::Graphic::System::TranslucentSortOrder;

    // Position of item @p index in the draw order.
    size_t DrawSlot( const std::vector<uint32_t>& order, uint32_t index )
    {
        for ( size_t slot = 0; slot < order.size(); ++slot )
            if ( order[slot] == index )
                return slot;
        ADD_FAILURE() << "item " << index << " missing from the draw order";
        return order.size();
    }
} // namespace

// Three panes in front of the camera, submitted near-to-far: drawn far-to-near.
TEST( TranslucentSortOrder, FartherDrawsFirst )
{
    const glm::vec3                        camera( 0.0f, 150.0f, 0.0f );
    const std::vector<TranslucentSortItem> items = {
         { .WorldBoundsCenter = { 0.0f, 150.0f, -100.0f } },  // near
         { .WorldBoundsCenter = { 0.0f, 150.0f, -1000.0f } }, // far
         { .WorldBoundsCenter = { 0.0f, 150.0f, -400.0f } },  // middle
    };
    const auto order = TranslucentSortOrder( camera, items );
    ASSERT_EQ( order.size(), items.size() );
    EXPECT_EQ( order, ( std::vector<uint32_t>{ 1, 2, 0 } ) );
}

// Distance is measured from the CAMERA, not from the origin: move the camera past the panes and the
// order reverses.
TEST( TranslucentSortOrder, DistanceIsFromTheCamera )
{
    const std::vector<TranslucentSortItem> items = {
         { .WorldBoundsCenter = { 0.0f, 0.0f, 0.0f } },
         { .WorldBoundsCenter = { 0.0f, 0.0f, 500.0f } },
    };
    EXPECT_LT( DrawSlot( TranslucentSortOrder( { 0.0f, 0.0f, -300.0f }, items ), 1 ),
               DrawSlot( TranslucentSortOrder( { 0.0f, 0.0f, -300.0f }, items ), 0 ) );
    EXPECT_LT( DrawSlot( TranslucentSortOrder( { 0.0f, 0.0f, 900.0f }, items ), 0 ),
               DrawSlot( TranslucentSortOrder( { 0.0f, 0.0f, 900.0f }, items ), 1 ) );
}

// Equal keys keep submission order, in both directions of submission.
TEST( TranslucentSortOrder, EqualKeysAreStable )
{
    const glm::vec3                        camera( 0.0f );
    const std::vector<TranslucentSortItem> items = {
         { .WorldBoundsCenter = { 200.0f, 0.0f, 0.0f } },
         { .WorldBoundsCenter = { -200.0f, 0.0f, 0.0f } }, // same distance, other side
         { .WorldBoundsCenter = { 0.0f, 200.0f, 0.0f } },
         { .WorldBoundsCenter = { 0.0f, 0.0f, 900.0f } }, // the one farther object
    };
    EXPECT_EQ( TranslucentSortOrder( camera, items ), ( std::vector<uint32_t>{ 3, 0, 1, 2 } ) );
}

// Priority overrides distance: a NEAR object with a lower priority draws before a FAR one with a higher
// priority (UE: lower TranslucencySortPriority draws behind), and within one priority distance decides.
TEST( TranslucentSortOrder, PriorityOverridesDistance )
{
    const glm::vec3                        camera( 0.0f );
    const std::vector<TranslucentSortItem> items = {
         { .WorldBoundsCenter = { 0.0f, 0.0f, 2000.0f }, .Priority = 1 }, // far, high priority
         { .WorldBoundsCenter = { 0.0f, 0.0f, 50.0f }, .Priority = -1 },  // near, low priority
         { .WorldBoundsCenter = { 0.0f, 0.0f, 900.0f }, .Priority = 0 },
         { .WorldBoundsCenter = { 0.0f, 0.0f, 100.0f }, .Priority = 1 }, // near, high priority
    };
    EXPECT_EQ( TranslucentSortOrder( camera, items ), ( std::vector<uint32_t>{ 1, 2, 0, 3 } ) );
}

TEST( TranslucentSortOrder, EmptyQueueIsEmptyOrder )
{
    EXPECT_TRUE( TranslucentSortOrder( glm::vec3( 0.0f ), {} ).empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

#include <gtest/gtest.h>

#include <SampleSubsystems.hpp>

#include <Common/Core/Events/EventTree.hpp>

using namespace Common;
using namespace SubsystemSamples;

namespace
{
    struct OwnerInTree
    {
        SampleOwner owner;
        EventTree   tree;
        EventNodeId node = tree.Attach<SampleOwner>( tree.Root(), owner );
    };
} // namespace

TEST( Subsystems, TheOwnersAnnotatedSubsystemsAreCreatedWithTheCollection )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    EXPECT_EQ( subsystems.Count(), 2u );
    EXPECT_NE( subsystems.Get<InputJournalSubsystem>(), nullptr );
    EXPECT_NE( subsystems.Get<ClockSubsystem>(), nullptr );
}

TEST( Subsystems, AnotherOwnersSubsystemIsNotCreated )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    EXPECT_EQ( subsystems.Get<ForeignOwnerSubsystem>(), nullptr );
}

TEST( Subsystems, ASubsystemIsConstructedWithItsOwner )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    ASSERT_NE( subsystems.Get<InputJournalSubsystem>(), nullptr );
    EXPECT_EQ( &subsystems.Get<InputJournalSubsystem>()->Owner(), &host.owner );
    EXPECT_EQ( &subsystems.GetOwner(), &host.owner );
}

TEST( Subsystems, ASubsystemWithHandlersIsANodeUnderItsOwner )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    const EventNodeId                node = subsystems.NodeOf<InputJournalSubsystem>();
    ASSERT_TRUE( node.IsSet() );
    EXPECT_TRUE( host.tree.Contains( node ) );
    EXPECT_EQ( host.tree.Parent( node ), host.node );
}

TEST( Subsystems, ASubsystemWithoutHandlersIsNotANode )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    EXPECT_FALSE( subsystems.NodeOf<ClockSubsystem>().IsSet() );
}

TEST( Subsystems, ABroadcastReachesASubsystemWithoutRegistration )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    EventWindowResize                resize( 800, 600 );
    host.tree.Route( resize );
    EXPECT_EQ( host.owner.At( host.owner.Recorded() - 1 ), "window resized" );
}

TEST( Subsystems, AFocusedSubsystemHandlesAKey )
{
    OwnerInTree                      host;
    SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    host.tree.SetFocus( subsystems.NodeOf<InputJournalSubsystem>() );
    KeyPressedEvent key( static_cast<KeyCode>( 65 ), 0 );
    EXPECT_TRUE( host.tree.Route( key ).Handled );
    EXPECT_EQ( host.owner.At( host.owner.Recorded() - 1 ), "key pressed" );
}

TEST( Subsystems, SubsystemsAreDestroyedWithTheCollectionInReverseOrder )
{
    OwnerInTree host;
    {
        SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
    }
    ASSERT_EQ( host.owner.Recorded(), 4u );
    EXPECT_EQ( host.owner.At( 0 ), "clock created" );
    EXPECT_EQ( host.owner.At( 1 ), "input journal created" );
    EXPECT_EQ( host.owner.At( 2 ), "input journal destroyed" );
    EXPECT_EQ( host.owner.At( 3 ), "clock destroyed" );
}

TEST( Subsystems, ADestroyedSubsystemLeavesTheTree )
{
    OwnerInTree host;
    EventNodeId node;
    {
        SubsystemCollection<SampleOwner> subsystems( host.owner, host.tree, host.node );
        node = subsystems.NodeOf<InputJournalSubsystem>();
    }
    EXPECT_FALSE( host.tree.Contains( node ) );
    EventWindowResize resize( 800, 600 );
    host.tree.Route( resize );
    EXPECT_EQ( host.owner.Recorded(), 4u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

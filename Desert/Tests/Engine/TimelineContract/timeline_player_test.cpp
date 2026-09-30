// THE TIMELINE CORE'S CONTRACT, group 4: the Player's time (ANIM-I5, Player.cpp).

#include "TimelineFixtures.hpp"

using namespace TimelineFixtures;

// ── 4. The Player ───────────────────────────────────────────────────────────────────────────────────

TEST( TimelinePlayer, NotPlayingIsAnEmptyStep )
{
    Player         player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    const TimeStep step = player.Advance( 0.5 );
    EXPECT_EQ( step.From.AsTicks(), step.To.AsTicks() );
    EXPECT_EQ( player.State(), PlayState::Stopped );
}

TEST( TimelinePlayer, LoopWrapsOnceAndReportsIt )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.SetLoopMode( LoopMode::Loop );
    player.Play();
    (void)player.Advance( 0.75 );                // tick 45
    const TimeStep step = player.Advance( 0.5 ); // 45 + 30 → wraps to 15
    EXPECT_TRUE( step.Wrapped );
    EXPECT_NEAR( player.Current().AsTicks(), 15.0, 1e-6 );
}

TEST( TimelinePlayer, OnceClampsOnTheLastTickAndStops )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.Play();
    const TimeStep step = player.Advance( 5.0 );
    EXPECT_TRUE( step.Finished );
    EXPECT_FALSE( step.Wrapped );
    EXPECT_EQ( player.Current().Frame, Tick( 60 ) );
    EXPECT_EQ( player.State(), PlayState::Stopped );
}

TEST( TimelinePlayer, PingPongTurnsAround )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.SetLoopMode( LoopMode::PingPong );
    player.Play();
    const TimeStep step = player.Advance( 1.25 ); // 75 → bounced back to 45
    EXPECT_TRUE( step.Reversed );
    EXPECT_NEAR( player.Current().AsTicks(), 45.0, 1e-6 );
}

TEST( TimelinePlayer, AJumpCrossesNothing )
{
    Player         player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    const TimeStep step = player.JumpTo( At( 40 ) );
    EXPECT_EQ( step.From.AsTicks(), step.To.AsTicks() );
    EXPECT_EQ( player.Current().Frame, Tick( 40 ) );
}

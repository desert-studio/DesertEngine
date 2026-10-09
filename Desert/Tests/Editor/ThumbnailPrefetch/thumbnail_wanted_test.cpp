// "A capture a tile keeps asking for stays queued; one nobody asks for leaves on the next tick."
//
// THUMB-FOLDER: the tiles of a 132-material folder asked once and then only painted their swatch; the
// service's next tick dropped every request no shower had renewed, and the folder made 1 picture in 3
// minutes. The rule lives in ThumbnailWanted so it is tested without a device.

#include <Editor/Widgets/ThumbnailWanted.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::Editor;

namespace
{
    struct Request
    {
        std::string Identity;
    };

    // One frame of the editor: the tiles draw (their asks), then the service ticks.
    void Tick( ThumbnailWanted& wanted, std::vector<Request>& queue )
    {
        wanted.DropUnwanted( queue );
        wanted.EndTick();
    }
} // namespace

TEST( ThumbnailWanted, ARequestRenewedEveryFrameSurvivesTwoTicks )
{
    ThumbnailWanted      wanted;
    std::vector<Request> queue;
    wanted.Ask( "mat" );
    wanted.Queue( "mat" );
    queue.push_back( { "mat" } );
    Tick( wanted, queue ); // the frame it was asked in

    ASSERT_TRUE( wanted.StillAsked( "mat" ) ); // the tile is drawn again
    Tick( wanted, queue );
    ASSERT_TRUE( wanted.StillAsked( "mat" ) );
    Tick( wanted, queue );

    ASSERT_EQ( queue.size(), 1u );
    EXPECT_TRUE( wanted.IsQueued( "mat" ) );
}

TEST( ThumbnailWanted, ARequestNobodyRenewsIsDroppedAndTheTileIsToldSo )
{
    ThumbnailWanted      wanted;
    std::vector<Request> queue;
    wanted.Ask( "mat" );
    wanted.Queue( "mat" );
    queue.push_back( { "mat" } );
    Tick( wanted, queue );
    Tick( wanted, queue ); // the tile was scrolled away: no ask this frame

    EXPECT_TRUE( queue.empty() );
    EXPECT_FALSE( wanted.IsQueued( "mat" ) );
    EXPECT_FALSE( wanted.StillAsked( "mat" ) ); // so the tile asks again in full
}

TEST( ThumbnailWanted, OnlyTheUnaskedLeaveTheQueue )
{
    ThumbnailWanted      wanted;
    std::vector<Request> queue{ { "a" }, { "b" }, { "c" } };
    for ( const auto& req : queue )
        wanted.Queue( req.Identity );
    wanted.Ask( "a" );
    wanted.Ask( "c" );
    Tick( wanted, queue );

    ASSERT_EQ( queue.size(), 2u );
    EXPECT_EQ( queue[0].Identity, "a" );
    EXPECT_EQ( queue[1].Identity, "c" );
    EXPECT_FALSE( wanted.IsQueued( "b" ) );
}

// StartupMoviePlayer — the launch sequence (UE's FDefaultGameMoviePlayer) against the suite's two clips:
// the order the project lists, the skip, a missing movie, and the game's readiness ending the sequence.
// No GPU and no audio device: the picture's destination is the host's, and the clock runs on Tick's delta.

#include <Engine/Media/StartupMoviePlayer.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace Desert::Media;

namespace
{
    const std::filesystem::path kRed     = DESERT_MEDIA_TEST_CLIP;    // 320x180, 1 s
    const std::filesystem::path kPattern = DESERT_MEDIA_PATTERN_CLIP; // 1920x1080, 5 s

    void TickFor( StartupMoviePlayer& movies, double seconds )
    {
        for ( double t = 0.0; t < seconds; t += 1.0 / 60.0 )
            movies.Tick( 1.0 / 60.0 );
    }
} // namespace

TEST( MediaPlayback, StartupMoviesPlayInTheListedOrderAndFinishAfterTheLast )
{
    StartupMoviePlayer movies( { { kRed, kPattern }, true, true } );
    movies.Start();
    ASSERT_FALSE( movies.Finished() );
    EXPECT_EQ( movies.CurrentIndex(), 0u );
    EXPECT_EQ( movies.Player().GetVideoWidth(), 320u );
    EXPECT_EQ( movies.Player().GetState(), MediaPlayerState::Playing );

    TickFor( movies, 0.5 ); // half of the first: still on it
    EXPECT_EQ( movies.CurrentIndex(), 0u );

    TickFor( movies, 1.0 ); // past its end: the second one plays, from its start
    ASSERT_EQ( movies.CurrentIndex(), 1u );
    EXPECT_EQ( movies.Player().GetVideoWidth(), 1920u );
    EXPECT_EQ( movies.Player().GetState(), MediaPlayerState::Playing );
    EXPECT_LT( movies.Player().GetTime(), 1.0 );

    TickFor( movies, 5.5 ); // and past the last: the sequence is over, the player closed
    EXPECT_TRUE( movies.Finished() );
    EXPECT_EQ( movies.CurrentIndex(), 2u );
    EXPECT_EQ( movies.Player().GetState(), MediaPlayerState::Closed );
}

TEST( MediaPlayback, StartupMovieSkipMovesToTheNextOnlyWhenSkippable )
{
    StartupMoviePlayer skippable( { { kPattern, kRed }, true, true } );
    skippable.Start();
    EXPECT_TRUE( skippable.Skip() );
    EXPECT_EQ( skippable.CurrentIndex(), 1u );
    EXPECT_EQ( skippable.Player().GetVideoWidth(), 320u );
    EXPECT_TRUE( skippable.Skip() );
    EXPECT_TRUE( skippable.Finished() );
    EXPECT_FALSE( skippable.Skip() ); // nothing left to skip

    StartupMoviePlayer unskippable( { { kRed, kRed }, false, true } );
    unskippable.Start();
    EXPECT_FALSE( unskippable.Skip() );
    EXPECT_EQ( unskippable.CurrentIndex(), 0u );
    EXPECT_EQ( unskippable.Player().GetState(), MediaPlayerState::Playing );
}

TEST( MediaPlayback, StartupMovieThatDoesNotOpenIsReportedAndPassedOver )
{
    const std::filesystem::path        missing = kRed.parent_path() / "no_such_startup_movie.webm";
    StartupMoviePlayer                 movies( { { missing, kRed, missing }, true, true } );
    std::vector<std::filesystem::path> failed;
    movies.OnMovieFailed = [&]( const std::filesystem::path& movie, const std::string& error )
    {
        failed.push_back( movie );
        EXPECT_FALSE( error.empty() );
    };
    movies.Start();
    ASSERT_EQ( failed.size(), 1u );
    EXPECT_EQ( movies.CurrentIndex(), 1u ); // the first one that opens plays

    TickFor( movies, 1.5 ); // the red clip ends, the last one is missing too: over
    EXPECT_EQ( failed.size(), 2u );
    EXPECT_TRUE( movies.Finished() );

    StartupMoviePlayer none( { { missing }, true, true } );
    none.Start();
    EXPECT_TRUE( none.Finished() );
    StartupMoviePlayer empty( {} );
    empty.Start();
    EXPECT_TRUE( empty.Finished() );
}

TEST( MediaPlayback, StartupMoviesEndWithTheLoadOnlyWhenNotWaitingForCompletion )
{
    StartupMoviePlayer waiting( { { kRed, kRed }, true, true } );
    waiting.Start();
    waiting.NotifyContentReady();
    EXPECT_FALSE( waiting.Finished() );
    EXPECT_EQ( waiting.CurrentIndex(), 0u );
    TickFor( waiting, 2.5 ); // both play to their ends regardless
    EXPECT_TRUE( waiting.Finished() );

    StartupMoviePlayer notWaiting( { { kRed, kRed }, true, false } );
    notWaiting.Start();
    TickFor( notWaiting, 0.25 );
    EXPECT_FALSE( notWaiting.Finished() ); // the game is not ready yet: the movie plays on
    notWaiting.NotifyContentReady();
    EXPECT_TRUE( notWaiting.Finished() );
    EXPECT_EQ( notWaiting.Player().GetState(), MediaPlayerState::Closed );
}

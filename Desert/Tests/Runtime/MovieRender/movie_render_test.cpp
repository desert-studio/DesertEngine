// MovieRender — the parse of `--render-movie` (Runtime/Source/MovieRender.hpp): every flag required once a
// movie is asked for, the frame count and step derived from one place, and frames named for ffmpeg's %05d.
// The byte-for-byte determinism of two real renders is checked by Games/Mirage/Content/Movies/Source/
// check_movie_determinism.sh (it needs the GPU and the Runtime binary, which a suite does not link).

#include "MovieRender.hpp"

#include <gtest/gtest.h>

using Desert::Player::ParseMovieRender;

namespace
{
    std::vector<std::string> Full()
    {
        return { "--render-movie", "Movies/Source/Title.desce", "--movie-out", "out", "--resolution", "64x36",
                 "--fps", "60", "--duration", "0.1666667" };
    }
} // namespace

TEST( MovieRender, NoMovieFlagsIsNoRequest )
{
    auto parsed = ParseMovieRender( { "--scene", "a.desce", "--shot", "x.png" } );
    ASSERT_TRUE( parsed.IsSuccess() );
    EXPECT_FALSE( parsed.GetValue().has_value() );
}

TEST( MovieRender, FullRequestParses )
{
    auto parsed = ParseMovieRender( Full() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& r = *parsed.GetValue();
    EXPECT_EQ( r.Map, "Movies/Source/Title.desce" );
    EXPECT_EQ( r.Width, 64u );
    EXPECT_EQ( r.Height, 36u );
    EXPECT_EQ( r.Fps, 60u );
    EXPECT_EQ( r.FrameCount(), 10u ); // 0.1666667 s at 60 fps
    EXPECT_FLOAT_EQ( r.FrameStep(), 1.0f / 60.0f );
    EXPECT_EQ( r.FramePath( 7 ).filename().string(), "00007.png" );
}

TEST( MovieRender, EveryFlagIsRequired )
{
    for ( const char* drop : { "--movie-out", "--resolution", "--fps", "--duration" } )
    {
        std::vector<std::string> args;
        const auto               full = Full();
        for ( size_t i = 0; i < full.size(); i += 2 )
            if ( full[i] != drop )
                args.insert( args.end(), { full[i], full[i + 1] } );
        auto parsed = ParseMovieRender( args );
        ASSERT_FALSE( parsed.IsSuccess() ) << "accepted without " << drop;
        EXPECT_NE( parsed.GetError().find( drop ), std::string::npos ) << parsed.GetError();
    }
    EXPECT_FALSE( ParseMovieRender( { "--fps", "60" } ).IsSuccess() ); // movie flag without --render-movie
}

TEST( MovieRender, MalformedValuesAreRefused )
{
    for ( const auto& [flag, value] : std::vector<std::pair<std::string, std::string>>{
               { "--resolution", "3840" }, { "--resolution", "0x10" }, { "--resolution", "axb" },
               { "--fps", "0" }, { "--fps", "6o" }, { "--duration", "-1" }, { "--duration", "3s" },
               { "--duration", "0.001" } } )
    {
        auto args = Full();
        for ( size_t i = 0; i < args.size(); i += 2 )
            if ( args[i] == flag )
                args[i + 1] = value;
        EXPECT_FALSE( ParseMovieRender( args ).IsSuccess() ) << flag << " " << value;
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

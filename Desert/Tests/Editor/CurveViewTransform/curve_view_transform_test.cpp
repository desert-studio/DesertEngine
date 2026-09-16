// THE CURVE VIEW'S ARITHMETIC, ASSERTED WITHOUT OPENING A WINDOW.
//
// `SequencerPanel.cpp` is compiled by no suite, and an ImGui frame is not a thing a test can open, so the
// only way any of a curve editor's behaviour becomes checkable is to put the arithmetic somewhere that has
// no ImGui in it. That is what `CurveView.hpp` is for, and this is what it buys.
//
// The assertions are about PROPERTIES rather than numbers, because the numbers are all viewport-dependent
// and a test that pinned them would have to be rewritten the first time a panel changed size:
//
//   * the two mappings round-trip;
//   * the value axis is FLIPPED (bigger value, smaller y) — the one sign every curve editor gets wrong;
//   * a slope survives the trip through pixels AND BACK IN A VIEWPORT OF A DIFFERENT SHAPE, which is the
//     property that says the two scales are both applied, in one place, in both directions;
//   * the handle a key draws is TANGENT TO THE CURVE THE EVALUATOR DRAWS — the cross-check between the
//     view and `EvaluateSegment`, i.e. between what the animator grabs and what playback does.

#include <Editor/Panels/Sequencer/CurveView.hpp>

#include <Engine/Animation/KeyInterpolation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace Anim = Desert::Animation;
using Desert::Editor::Sequencer::ChooseFrameStep;
using Desert::Editor::Sequencer::CurveViewport;
using Desert::Editor::Sequencer::FitValueRange;

namespace
{
    /// A viewport of a deliberately awkward shape: not square, not starting at the origin, and with a value
    /// range whose units are nothing like its pixels.
    CurveViewport MakeViewport()
    {
        CurveViewport vp;
        vp.X0        = 120.0F;
        vp.X1        = 920.0F;
        vp.Y0        = 60.0F;  // top
        vp.Y1        = 460.0F; // bottom
        vp.TimeStart = 0.0;
        vp.TimeEnd   = 2.0;
        vp.ValueMin  = -50.0F;
        vp.ValueMax  = 250.0F;
        return vp;
    }

    Anim::ScalarKey Key( int32_t tick, float value, Anim::KeyInterp interp = Anim::KeyInterp::Cubic )
    {
        Anim::ScalarKey k;
        k.Tick   = Anim::FrameNumber{ tick };
        k.Value  = value;
        k.Interp = interp;
        k.Mode   = Anim::TangentMode::Auto;
        return k;
    }
} // namespace

TEST( CurveViewTransform, TimeAndValueRoundTripThroughPixels )
{
    const CurveViewport vp = MakeViewport();

    for ( double seconds = 0.0; seconds <= 2.0; seconds += 0.125 )
    {
        EXPECT_NEAR( vp.XToTime( vp.TimeToX( seconds ) ), seconds, 1e-4 );
    }
    for ( float value = -50.0F; value <= 250.0F; value += 12.5F )
    {
        EXPECT_NEAR( vp.YToValue( vp.ValueToY( value ) ), value, 1e-2F );
    }
}

TEST( CurveViewTransform, TheValueAxisPointsUpTheScreen )
{
    const CurveViewport vp = MakeViewport();

    // THE SIGN EVERY CURVE EDITOR GETS WRONG ONCE. A bigger value must be HIGHER on screen, i.e. a smaller
    // y, because ImGui's y grows downward. Without this the whole curve is drawn upside down and every
    // tangent handle points the wrong way, which reads as "the maths is broken" rather than "the axis is".
    EXPECT_LT( vp.ValueToY( 200.0F ), vp.ValueToY( 0.0F ) ) << "a larger value is not drawn higher";
    EXPECT_FLOAT_EQ( vp.ValueToY( vp.ValueMin ), vp.Y1 ) << "the minimum is not on the bottom edge";
    EXPECT_FLOAT_EQ( vp.ValueToY( vp.ValueMax ), vp.Y0 ) << "the maximum is not on the top edge";
}

TEST( CurveViewTransform, ASlopeSurvivesPixelsInAViewportOfAnyShape )
{
    const CurveViewport wide = MakeViewport();
    CurveViewport       tall = MakeViewport();
    tall.Y1                  = 1460.0F; // four times the pixels per value unit
    tall.TimeEnd             = 8.0;     // and a quarter of the pixels per second
    tall.ValueMax            = 1050.0F;

    for ( const float slope : { -400.0F, -37.5F, 0.0F, 12.25F, 480.0F } )
    {
        for ( const bool leaving : { true, false } )
        {
            const auto wideBack = wide.SlopeFromHandle( wide.HandleOffset( slope, 55.0F, leaving ), leaving );
            const auto tallBack = tall.SlopeFromHandle( tall.HandleOffset( slope, 55.0F, leaving ), leaving );

            ASSERT_TRUE( wideBack.has_value() );
            ASSERT_TRUE( tallBack.has_value() );
            EXPECT_NEAR( wideBack.value_or( 0.0F ), slope, std::abs( slope ) * 1e-3F + 1e-3F );
            // THE SAME NUMBER OUT OF A DIFFERENTLY SHAPED BOX. If either scale were missing from one of the
            // two directions, this is where it shows: the round trip would still close in the viewport it
            // was written against and open in every other one.
            EXPECT_NEAR( tallBack.value_or( 0.0F ), slope, std::abs( slope ) * 1e-3F + 1e-3F );
        }
    }
}

TEST( CurveViewTransform, APositiveSlopePointsUpAndTheTwoHandlesMirror )
{
    const CurveViewport vp = MakeViewport();

    const glm::vec2 rising = vp.HandleOffset( 120.0F, 50.0F, true );
    EXPECT_GT( rising.x, 0.0F ) << "the leaving handle does not point forward in time";
    EXPECT_LT( rising.y, 0.0F ) << "a rising tangent does not point up the screen";

    const glm::vec2 arriving = vp.HandleOffset( 120.0F, 50.0F, false );
    EXPECT_NEAR( arriving.x, -rising.x, 1e-3F );
    EXPECT_NEAR( arriving.y, -rising.y, 1e-3F ) << "the two handles of one slope are not a straight line";
}

TEST( CurveViewTransform, TheHandleIsTheSameLengthWhateverTheSlope )
{
    const CurveViewport vp = MakeViewport();

    // A DECISION, NOT AN ARTEFACT: length is reserved for weights (A6 shipped them as zero), so today it
    // carries nothing and must stay constant. A handle whose length grew with the slope leaves the panel
    // for a steep key, and the animator is then dragging something they cannot see.
    for ( const float slope : { -900.0F, -1.0F, 0.0F, 3.5F, 2000.0F } )
    {
        const glm::vec2 h = vp.HandleOffset( slope, 64.0F, true );
        EXPECT_NEAR( std::sqrt( h.x * h.x + h.y * h.y ), 64.0F, 1e-2F ) << "slope " << slope;
    }
}

TEST( CurveViewTransform, AHandleDraggedBehindItsKeyIsRefusedRatherThanMadeInfinite )
{
    const CurveViewport vp = MakeViewport();

    // dx <= 0 on a leaving handle is not a steeper slope; it is a division by zero one pixel away. The
    // arithmetic answer would be an infinity, and it would be WRITTEN TO THE FILE.
    EXPECT_FALSE( vp.SlopeFromHandle( glm::vec2( 0.0F, -40.0F ), true ).has_value() );
    EXPECT_FALSE( vp.SlopeFromHandle( glm::vec2( -30.0F, -40.0F ), true ).has_value() );
    EXPECT_FALSE( vp.SlopeFromHandle( glm::vec2( 0.0F, -40.0F ), false ).has_value() );
    EXPECT_TRUE( vp.SlopeFromHandle( glm::vec2( 30.0F, -40.0F ), true ).has_value() );
}

TEST( CurveViewTransform, TheHandleIsTangentToTheCurveTheEvaluatorDraws )
{
    // THE CROSS-CHECK BETWEEN THE VIEW AND PLAYBACK. Everything above is internally consistent arithmetic;
    // this is the one that fails if the view's idea of a tangent and `EvaluateSegment`'s disagree — i.e. if
    // the thing the animator grabs is not the thing the curve does.
    //
    // THE DIFFERENCE IS ONE-SIDED AND SECOND-ORDER, and the first version of this test was not — it used
    // (f(h) - f(0))/h at a key whose tangent the auto pass had made FLAT, so what it measured was the
    // segment's curvature (3(P2-P0)h/span) and it reported 0.18 where the slope is 0. A flat key is the
    // most important probe here, because it is the one the extremum rule creates, so the instrument has to
    // be able to read a zero rather than the test having to avoid one.
    const auto slopeOfSegment = []( const Anim::ScalarKey& from, const Anim::ScalarKey& to, double spanSeconds )
    {
        constexpr float step = 1e-3F;
        const auto      at   = [&]( float u )
        {
            return Anim::EvaluateSegment( from.Value, from.LeaveTangent, to.Value, to.ArriveTangent, to.Interp,
                                          spanSeconds, u );
        };
        return static_cast<float>( ( -3.0 * at( 0.0F ) + 4.0 * at( step ) - at( 2.0F * step ) ) /
                                   ( 2.0 * static_cast<double>( step ) * spanSeconds ) );
    };

    const Anim::FrameRate rate = Anim::PROJECT_TICK_RATE;
    const double          spanSeconds =
         Anim::FrameTimeToSeconds( Anim::FrameTime{ Anim::FrameNumber{ 24000 }, 0.0F }, rate );
    const CurveViewport vp = MakeViewport();

    const auto check = [&]( std::vector<Anim::ScalarKey> keys, const char* what ) -> float
    {
        Anim::AutoSetTangents( keys, rate );
        const auto recovered = vp.SlopeFromHandle( vp.HandleOffset( keys[1].LeaveTangent, 50.0F, true ), true );
        EXPECT_TRUE( recovered.has_value() ) << what;
        if ( !recovered.has_value() )
        {
            return 0.0F;
        }
        const float numeric = slopeOfSegment( keys[1], keys[2], spanSeconds );
        EXPECT_NEAR( recovered.value_or( 0.0F ), numeric, std::abs( numeric ) * 0.02F + 0.5F )
             << what << ": the handle the animator grabs is not tangent to the curve the evaluator draws";
        return numeric;
    };

    // A PEAK, where the auto pass makes the tangent flat — the handle must lie along the curve's own zero.
    check( { Key( 0, 0.0F ), Key( 24000, 100.0F ), Key( 48000, 40.0F ) }, "peak" );

    // AND A KEY THE CURVE IS ACTUALLY CLIMBING THROUGH, so the test cannot pass by everything being zero.
    const float rising = check( { Key( 0, 0.0F ), Key( 24000, 100.0F ), Key( 48000, 300.0F ) }, "rising" );
    EXPECT_GT( rising, 10.0F ) << "the rising probe is not actually rising, so it proves nothing";
}

TEST( CurveViewTransform, AFlatChannelGetsARangeRatherThanADivisionByZero )
{
    // Most channels in a clip never move (every bone's scale), so this is the common case, not an edge one.
    const std::vector<Anim::ScalarKey> flat{ Key( 0, 1.0F ), Key( 24000, 1.0F ) };
    const glm::vec2                    range = FitValueRange( flat, 0.1F );
    EXPECT_GT( range.y, range.x ) << "a flat channel produced a degenerate range";
    EXPECT_NEAR( ( range.x + range.y ) * 0.5F, 1.0F, 1e-4F ) << "the flat line is not in the middle";

    EXPECT_GT( FitValueRange( {}, 0.1F ).y, FitValueRange( {}, 0.1F ).x ) << "an empty channel is degenerate";

    const std::vector<Anim::ScalarKey> moving{ Key( 0, 0.0F ), Key( 24000, 10.0F ) };
    const glm::vec2                    padded = FitValueRange( moving, 0.1F );
    EXPECT_LT( padded.x, 0.0F ) << "the lowest key sits on the very edge of the box";
    EXPECT_GT( padded.y, 10.0F ) << "the highest key sits on the very edge of the box";
}

TEST( CurveViewTransform, TheFrameGridThinsOutInsteadOfDrawingAThousandLines )
{
    // Never zero — a zero step is an infinite loop in the caller, and the caller is a draw loop.
    EXPECT_GE( ChooseFrameStep( 0.0, 8.0F ), 1 );
    EXPECT_GE( ChooseFrameStep( -4.0, 8.0F ), 1 );
    EXPECT_GE( ChooseFrameStep( 40.0, 0.0F ), 1 );

    // Room for every frame: step 1.
    EXPECT_EQ( ChooseFrameStep( 30.0, 8.0F ), 1 );

    // The guarantee, stated as a property rather than a table: whatever the zoom, the chosen step leaves at
    // least the asked-for gap, and it is the 1-2-5-10 ladder so the labels stay round.
    for ( double px = 0.01; px < 40.0; px *= 1.3 )
    {
        const int32_t step = ChooseFrameStep( px, 9.0F );
        EXPECT_GE( static_cast<double>( step ) * px, 9.0 - 1e-9 ) << "pixels per frame " << px;

        int32_t reduced = step;
        while ( reduced % 10 == 0 )
        {
            reduced /= 10;
        }
        EXPECT_TRUE( reduced == 1 || reduced == 2 || reduced == 5 )
             << "step " << step << " is not on the 1-2-5 ladder";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

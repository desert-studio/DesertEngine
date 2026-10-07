// UIPathStroke — the logo line of UIPath (Engine/UI/UIPathGeometry + DrawList2D::AddPolyline): Reveal is a
// fraction of the ARC LENGTH, so 0 draws nothing, 0.5 half the length and 1 all of it, on a curve whose
// spans are deliberately uneven (a parameter-based reveal would fail the half); and the stroke the draw
// list emits for it reaches exactly as far as the revealed prefix, with a fringe that fades to nothing.

#include <Engine/Graphic/Render2D/DrawList2D.hpp>
#include <Engine/UI/UIPathGeometry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace Desert;

namespace
{
    // Two short spans and one long one: the length is NOT evenly spread over the control points.
    std::vector<glm::vec2> Uneven()
    {
        return { { 0.0f, 0.0f }, { 10.0f, 5.0f }, { 20.0f, 0.0f }, { 300.0f, 40.0f } };
    }
} // namespace

TEST( UIPathStroke, RevealZeroHalfAndOneAreZeroHalfAndAllOfTheLength )
{
    for ( const bool smooth : { false, true } )
    {
        const auto points = Uneven();
        const auto line   = UI::TessellateUIPath( points, smooth );
        ASSERT_GT( line.Length, 0.0f );

        EXPECT_TRUE( UI::RevealUIPath( line, 0.0f ).empty() ) << "smooth=" << smooth;
        EXPECT_NEAR( UI::PolylineLength( UI::RevealUIPath( line, 0.5f ) ), 0.5f * line.Length,
                     1e-3f * line.Length )
             << "smooth=" << smooth;
        EXPECT_NEAR( UI::PolylineLength( UI::RevealUIPath( line, 1.0f ) ), line.Length, 1e-3f * line.Length )
             << "smooth=" << smooth;
        // Out of range clamps rather than extrapolating past the ends.
        EXPECT_NEAR( UI::PolylineLength( UI::RevealUIPath( line, 2.0f ) ), line.Length, 1e-3f * line.Length );
    }
}

TEST( UIPathStroke, TheSmoothCurvePassesThroughEveryControlPoint )
{
    const auto points = Uneven();
    const auto line   = UI::TessellateUIPath( points, true, 8 );
    for ( const glm::vec2& p : points )
    {
        const bool hit = std::any_of( line.Points.begin(), line.Points.end(),
                                      [&]( const glm::vec2& q ) { return glm::length( q - p ) < 1e-3f; } );
        EXPECT_TRUE( hit ) << p.x << "," << p.y;
    }
    EXPECT_GE( line.Length, UI::PolylineLength( points ) - 1e-3f ); // a curve through them is never shorter
}

TEST( UIPathStroke, TheEmittedStrokeEndsWhereTheRevealEnds )
{
    const std::vector<glm::vec2> points = { { 0.0f, 50.0f }, { 200.0f, 50.0f } };
    const auto                   line   = UI::TessellateUIPath( points, false );
    const auto                   shown  = UI::RevealUIPath( line, 0.5f );
    ASSERT_EQ( shown.size(), 2u );

    Graphic::Render2D::DrawList2D dl;
    dl.AddPolyline( shown.data(), static_cast<uint32_t>( shown.size() ), glm::vec4( 1.0f ), 4.0f, 1.0f,
                    /*roundCaps*/ false );
    ASSERT_FALSE( dl.GetVertices().empty() );

    float maxX       = -1e9f;
    float maxOpaqueX = -1e9f;
    bool  anyClear   = false;
    for ( const auto& v : dl.GetVertices() )
    {
        maxX = std::max( maxX, v.Position.x );
        if ( v.Color.a > 0.99f )
            maxOpaqueX = std::max( maxOpaqueX, v.Position.x );
        anyClear = anyClear || v.Color.a < 0.01f;
    }
    EXPECT_NEAR( maxX, 100.0f, 1e-3f );       // butt end: half of 200 px, nothing past it
    EXPECT_NEAR( maxOpaqueX, 100.0f, 1e-3f ); // the core reaches the end too
    EXPECT_TRUE( anyClear );                  // the antialias fringe fades to zero

    Graphic::Render2D::DrawList2D capped;
    capped.AddPolyline( shown.data(), 2u, glm::vec4( 1.0f ), 4.0f, 1.0f, /*roundCaps*/ true );
    float cappedMaxX = -1e9f;
    for ( const auto& v : capped.GetVertices() )
        cappedMaxX = std::max( cappedMaxX, v.Position.x );
    EXPECT_NEAR( cappedMaxX, 100.0f + 2.0f + 1.0f, 1e-2f ); // the round cap adds half the width plus fringe
}

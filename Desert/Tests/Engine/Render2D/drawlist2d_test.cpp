// Unit tests for the pure CPU 2D draw-list geometry builder (the batcher's front end). No GPU / Vulkan /
// ECS — just verifies that primitives emit the expected vertices, indices and state batches.

#include <Render2DCore/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/PreparedDraws.hpp>
#include <Engine/Graphic/Render2D/UIMaterialFallback.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using Desert::Graphic::Render2D::DrawList2D;
namespace R2D = Desert::Graphic::Render2D;

namespace
{
    constexpr float kEps = 1e-4f;
}

TEST( DrawList2D, StartsEmpty )
{
    DrawList2D dl;
    EXPECT_TRUE( dl.Empty() );
    EXPECT_TRUE( dl.GetVertices().empty() );
    EXPECT_TRUE( dl.GetIndices().empty() );
    EXPECT_TRUE( dl.GetCommands().empty() );
}

TEST( DrawList2D, AddRectFilledEmitsQuad )
{
    DrawList2D dl;
    dl.AddRectFilled( { 10.0f, 20.0f }, { 110.0f, 70.0f }, { 0.2f, 0.4f, 0.6f, 1.0f } );

    ASSERT_EQ( dl.GetVertices().size(), 4u );
    ASSERT_EQ( dl.GetIndices().size(), 6u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_FALSE( dl.Empty() );

    const auto& v = dl.GetVertices();
    // Corner order: TL, TR, BR, BL.
    EXPECT_NEAR( v[0].Position.x, 10.0f, kEps );
    EXPECT_NEAR( v[0].Position.y, 20.0f, kEps );
    EXPECT_NEAR( v[2].Position.x, 110.0f, kEps );
    EXPECT_NEAR( v[2].Position.y, 70.0f, kEps );

    // UVs span the full 0..1 range so a sprite/atlas maps edge to edge.
    EXPECT_NEAR( v[0].UV.x, 0.0f, kEps );
    EXPECT_NEAR( v[0].UV.y, 0.0f, kEps );
    EXPECT_NEAR( v[2].UV.x, 1.0f, kEps );
    EXPECT_NEAR( v[2].UV.y, 1.0f, kEps );

    // Colour is carried per-vertex.
    for ( const auto& vert : v )
    {
        EXPECT_NEAR( vert.Color.r, 0.2f, kEps );
        EXPECT_NEAR( vert.Color.g, 0.4f, kEps );
        EXPECT_NEAR( vert.Color.b, 0.6f, kEps );
        EXPECT_NEAR( vert.Color.a, 1.0f, kEps );
    }

    // Two triangles referencing the four corners, fanned from corner 0.
    //
    // THE SECOND TRIANGLE USED TO BE WRITTEN {2,3,0} AND IS NOW {0,2,3}, which is the same three corners in
    // the same cyclic order — same triangle, same winding, same pixels — and the change is not cosmetic.
    // Every fan triangle now starts at corner 0, so the diagonal 0->2 is traversed in ONE direction by both
    // triangles. When a rotated clipper cuts a quad (Ю9) each triangle is cut separately, and two triangles
    // that met that diagonal from opposite ends would split it at two floats one ULP apart — a crack the
    // rasterizer can sample through. Fanning from a single corner removes the possibility.
    const std::vector<uint32_t> expected = { 0, 1, 2, 0, 2, 3 };
    EXPECT_EQ( dl.GetIndices(), expected );

    const auto& cmd = dl.GetCommands()[0];
    EXPECT_EQ( cmd.Texture, nullptr ); // solid => white texture
    EXPECT_EQ( cmd.IndexOffset, 0u );
    EXPECT_EQ( cmd.IndexCount, 6u );
}

TEST( DrawList2D, ConsecutiveSolidRectsMergeIntoOneBatch )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1, 1, 1, 1 } );
    dl.AddRectFilled( { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1, 0, 0, 1 } );

    EXPECT_EQ( dl.GetVertices().size(), 8u );
    EXPECT_EQ( dl.GetIndices().size(), 12u );
    // Same state (white texture) => a single draw command spanning both quads.
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, 12u );

    // Second quad's indices are offset by its base vertex (4).
    const auto& idx = dl.GetIndices();
    EXPECT_EQ( idx[6], 4u );
    EXPECT_EQ( idx[8], 6u );
}

TEST( DrawList2D, ResetClearsGeometryKeepsUsable )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    dl.Reset();

    EXPECT_TRUE( dl.Empty() );
    EXPECT_TRUE( dl.GetCommands().empty() );

    dl.AddRectFilled( { 0, 0 }, { 2, 2 }, { 1, 1, 1, 1 } );
    EXPECT_EQ( dl.GetVertices().size(), 4u );
    EXPECT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetIndices()[0], 0u ); // indices re-based after reset
}

TEST( DrawList2D, AddImageCarriesUVsAndTexture )
{
    DrawList2D  dl;
    int         tex = 0; // any non-null opaque id
    const void* id  = &tex;
    dl.AddImage( id, { 0.0f, 0.0f }, { 100.0f, 100.0f }, { 0.25f, 0.5f }, { 0.75f, 1.0f }, { 1, 1, 1, 1 } );

    ASSERT_EQ( dl.GetVertices().size(), 4u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].Texture, id );

    const auto& v = dl.GetVertices();
    EXPECT_NEAR( v[0].UV.x, 0.25f, kEps ); // TL = uv0
    EXPECT_NEAR( v[0].UV.y, 0.5f, kEps );
    EXPECT_NEAR( v[2].UV.x, 0.75f, kEps ); // BR = uv1
    EXPECT_NEAR( v[2].UV.y, 1.0f, kEps );
    EXPECT_NEAR( v[1].UV.x, 0.75f, kEps ); // TR = (uv1.x, uv0.y)
    EXPECT_NEAR( v[1].UV.y, 0.5f, kEps );
}

TEST( DrawList2D, DifferentTexturesSplitBatches )
{
    DrawList2D dl;
    int        a = 0, b = 0;

    dl.AddRectFilled( { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } ); // solid (null texture)
    dl.AddImage( &a, { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    dl.AddImage( &b, { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );

    // Three distinct textures (null, &a, &b) => three commands, each 6 indices at increasing offsets.
    ASSERT_EQ( dl.GetCommands().size(), 3u );
    EXPECT_EQ( dl.GetCommands()[0].Texture, nullptr );
    EXPECT_EQ( dl.GetCommands()[1].Texture, &a );
    EXPECT_EQ( dl.GetCommands()[2].Texture, &b );
    EXPECT_EQ( dl.GetCommands()[0].IndexOffset, 0u );
    EXPECT_EQ( dl.GetCommands()[1].IndexOffset, 6u );
    EXPECT_EQ( dl.GetCommands()[2].IndexOffset, 12u );
}

TEST( DrawList2D, AddTextMarksBatchAndSplitsFromImage )
{
    DrawList2D dl;
    int        atlas = 0;

    // Same texture id, but image vs text are distinct GPU states (different pipeline) => two batches.
    dl.AddImage( &atlas, { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    dl.AddText( &atlas, { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    dl.AddText( &atlas, { 2, 0 }, { 3, 1 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );

    ASSERT_EQ( dl.GetCommands().size(), 2u );
    EXPECT_FALSE( dl.GetCommands()[0].Text );
    EXPECT_TRUE( dl.GetCommands()[1].Text );
    EXPECT_EQ( dl.GetCommands()[1].Texture, &atlas );
    // The two glyph quads share one text batch.
    EXPECT_EQ( dl.GetCommands()[1].IndexCount, 12u );
}

TEST( DrawList2D, MultiColorRectGradesTopToBottom )
{
    DrawList2D dl;
    dl.AddRectFilledMultiColor( { 0, 0 }, { 10, 10 }, { 1, 0, 0, 1 }, { 0, 0, 1, 1 } );

    ASSERT_EQ( dl.GetVertices().size(), 4u );
    const auto& v = dl.GetVertices();
    EXPECT_NEAR( v[0].Color.r, 1.0f, kEps ); // TL top colour
    EXPECT_NEAR( v[1].Color.r, 1.0f, kEps ); // TR top colour
    EXPECT_NEAR( v[2].Color.b, 1.0f, kEps ); // BR bottom colour
    EXPECT_NEAR( v[3].Color.b, 1.0f, kEps ); // BL bottom colour
}

TEST( DrawList2D, RectOutlineEmitsFourBars )
{
    DrawList2D dl;
    dl.AddRect( { 0, 0 }, { 100, 50 }, { 1, 1, 1, 1 }, 2.0f );

    // Four filled bars, all solid (white) => one merged batch of 4 quads.
    EXPECT_EQ( dl.GetVertices().size(), 16u );
    EXPECT_EQ( dl.GetIndices().size(), 24u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, 24u );
}

TEST( DrawList2D, ClipRectSplitsBatchAndRestores )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } ); // unclipped -> cmd 0
    dl.PushClipRect( { 0, 0 }, { 5, 5 } );
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } ); // clipped -> cmd 1
    dl.PopClipRect();
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } ); // unclipped again -> cmd 2

    ASSERT_EQ( dl.GetCommands().size(), 3u );
    EXPECT_LE( dl.GetCommands()[0].ClipRect.z, 0.0f ); // no clip
    EXPECT_NEAR( dl.GetCommands()[1].ClipRect.z, 5.0f, kEps );
    EXPECT_NEAR( dl.GetCommands()[1].ClipRect.w, 5.0f, kEps );
    EXPECT_LE( dl.GetCommands()[2].ClipRect.z, 0.0f );
}

TEST( DrawList2D, NestedClipIntersects )
{
    DrawList2D dl;
    dl.PushClipRect( { 0, 0 }, { 100, 100 } );
    dl.PushClipRect( { 50, 50 }, { 200, 200 } ); // intersect -> (50,50)-(100,100)
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } );
    dl.PopClipRect();
    dl.PopClipRect();

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    const auto& clip = dl.GetCommands()[0].ClipRect;
    EXPECT_NEAR( clip.x, 50.0f, kEps );
    EXPECT_NEAR( clip.y, 50.0f, kEps );
    EXPECT_NEAR( clip.z, 50.0f, kEps ); // width 100-50
    EXPECT_NEAR( clip.w, 50.0f, kEps );
}

TEST( DrawList2D, RoundedRectFansFromCentre )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 100, 100 }, { 1, 1, 1, 1 }, 12.0f );

    // A 12 px corner stays at the 6-segment floor: 4 * 7 perimeter vertices. Centre + perimeter for the
    // fan, then the closed fringe strip: (perimeter + 1) outer/inner pairs.
    ASSERT_EQ( DrawList2D::RoundedCornerSegments( 12.0f ), 6 );
    const uint32_t perim = 4u * 7u;
    EXPECT_EQ( dl.GetVertices().size(), 1u + perim + 2u * ( perim + 1u ) );
    // One fan triangle per perimeter vertex, two fringe triangles per perimeter edge.
    EXPECT_EQ( dl.GetIndices().size(), perim * 3u + perim * 6u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].Texture, nullptr );
}

// A disc the size of a dune (r = 2200 px at 4K) was a 24-gon: six chords per quarter, each straying ~45 px
// from the arc. Every chord of the solid rim must now stay within kArcError of the inner circle, and the
// fringe must fade from opaque at r - 0.5 to clear at r + 0.5.
TEST( DrawList2D, LargeRoundedDiscStaysRoundAndFeathered )
{
    const float     r = 2200.0f;
    const glm::vec2 c = { 2200.0f, 2200.0f };
    DrawList2D      dl;
    dl.AddRectFilled( { 0, 0 }, { 2 * r, 2 * r }, { 1, 0.5f, 0.25f, 1 }, r );

    const int      seg   = DrawList2D::RoundedCornerSegments( r );
    const uint32_t perim = 4u * static_cast<uint32_t>( seg + 1 );
    const auto&    v     = dl.GetVertices();
    ASSERT_EQ( v.size(), 1u + perim + 2u * ( perim + 1u ) );

    const float inner = r - DrawList2D::kEdgeFringe * 0.5f;
    for ( uint32_t i = 0; i < perim; ++i )
    {
        const glm::vec2 a   = v[1 + i].Position;
        const glm::vec2 b   = v[1 + ( i + 1 ) % perim].Position;
        const float     mid = glm::length( ( a + b ) * 0.5f - c );
        EXPECT_NEAR( glm::length( a - c ), inner, 1e-2f ) << "rim vertex " << i;
        EXPECT_GE( mid, inner - DrawList2D::kArcError - 1e-2f ) << "chord " << i << " strays from the arc";
        EXPECT_EQ( v[1 + i].Color.a, 1.0f );
    }
    for ( uint32_t i = 0; i <= perim; ++i )
    {
        const auto& outer = v[1 + perim + i * 2];
        EXPECT_NEAR( glm::length( outer.Position - c ), r + DrawList2D::kEdgeFringe * 0.5f, 1e-2f );
        EXPECT_EQ( outer.Color.a, 0.0f ) << "fringe vertex " << i << " is not clear";
        EXPECT_EQ( v[1 + perim + i * 2 + 1].Color.a, 1.0f );
    }
}

// The sibling drawn after a Retainer Box (a dune after the masked sun) went into the composite's command:
// texture null, not text, same scissor — so it merged, was drawn with the layer's picture, and vanished.
TEST( DrawList2D, AFillAfterARetainedCompositeGetsItsOwnCommand )
{
    DrawList2D dl;
    uint32_t   layer = 0;
    dl.BeginRetainedLayer( &layer ).AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } );
    ASSERT_TRUE( dl.AddRetainedComposite( layer, -1, {}, glm::vec4( 1.0f ) ) );
    dl.AddRectFilled( { 20, 20 }, { 40, 40 }, { 0, 0, 0, 1 }, 6.0f );

    ASSERT_EQ( dl.GetCommands().size(), 2u );
    EXPECT_TRUE( dl.GetCommands()[0].Retained );
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, 6u ) << "the fill was appended to the composite's quad";
    EXPECT_FALSE( dl.GetCommands()[1].Retained );
    EXPECT_GT( dl.GetCommands()[1].IndexCount, 0u );
}

TEST( DrawList2D, ZeroRoundingStaysSharpQuad )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 }, 0.0f );
    EXPECT_EQ( dl.GetVertices().size(), 4u ); // sharp path
    EXPECT_EQ( dl.GetIndices().size(), 6u );
}

// --- Glass (backdrop blur) ------------------------------------------------------------------------
// A glass rect carries its rect / radius / blur in push constants, so it must ALWAYS get its own draw
// command: merging it with a neighbour would silently draw that neighbour with this rect's parameters.

TEST( DrawList2D, GlassRectEmitsOwnCommandWithParams )
{
    DrawList2D dl;
    dl.AddGlassRect( { 10.0f, 20.0f }, { 110.0f, 70.0f }, { 0.1f, 0.2f, 0.3f, 0.4f }, 8.0f, 0.5f );

    ASSERT_EQ( dl.GetVertices().size(), 4u );
    ASSERT_EQ( dl.GetIndices().size(), 6u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );

    const auto& cmd = dl.GetCommands()[0];
    EXPECT_TRUE( cmd.Glass );
    EXPECT_FALSE( cmd.Text );
    EXPECT_EQ( cmd.Texture, nullptr );
    EXPECT_EQ( cmd.IndexOffset, 0u );
    EXPECT_EQ( cmd.IndexCount, 6u );
    EXPECT_NEAR( cmd.GlassRect.x, 10.0f, kEps );
    EXPECT_NEAR( cmd.GlassRect.y, 20.0f, kEps );
    EXPECT_NEAR( cmd.GlassRect.z, 110.0f, kEps );
    EXPECT_NEAR( cmd.GlassRect.w, 70.0f, kEps );
    EXPECT_NEAR( cmd.GlassRound, 8.0f, kEps );
    EXPECT_NEAR( cmd.GlassLod, 0.5f, kEps );

    // The tint reaches the shader through the vertex colour.
    EXPECT_NEAR( dl.GetVertices()[0].Color.a, 0.4f, kEps );
}

TEST( DrawList2D, GlassNeverMergesWithNeighbours )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddGlassRect( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 0.2f } );
    dl.AddGlassRect( { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 0.2f } );
    dl.AddRectFilled( { 40.0f, 0.0f }, { 50.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    // solid | glass | glass | solid — four separate commands, each anchored at its own indices.
    ASSERT_EQ( dl.GetCommands().size(), 4u );
    EXPECT_FALSE( dl.GetCommands()[0].Glass );
    EXPECT_TRUE( dl.GetCommands()[1].Glass );
    EXPECT_TRUE( dl.GetCommands()[2].Glass );
    EXPECT_FALSE( dl.GetCommands()[3].Glass );
    for ( uint32_t i = 0; i < 4; ++i )
    {
        EXPECT_EQ( dl.GetCommands()[i].IndexOffset, i * 6u );
        EXPECT_EQ( dl.GetCommands()[i].IndexCount, 6u );
    }
}

TEST( DrawList2D, GlassClampsBlurAndIgnoresDegenerateRects )
{
    DrawList2D dl;
    dl.AddGlassRect( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, -4.0f, 3.0f );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_NEAR( dl.GetCommands()[0].GlassRound, 0.0f, kEps ); // negative radius is meaningless
    EXPECT_NEAR( dl.GetCommands()[0].GlassLod, 1.0f, kEps );   // blur is a 0..1 dial

    // An empty (or inverted) rect draws nothing at all — no stray command, no stray geometry.
    dl.AddGlassRect( { 50.0f, 50.0f }, { 50.0f, 80.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddGlassRect( { 90.0f, 50.0f }, { 10.0f, 80.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    EXPECT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetVertices().size(), 4u );
}

TEST( DrawList2D, TriangleEmitsOneTri )
{
    DrawList2D dl;
    dl.AddTriangleFilled( { 0, 0 }, { 10, 0 }, { 5, 10 }, { 1, 1, 1, 1 } );
    EXPECT_EQ( dl.GetVertices().size(), 3u );
    EXPECT_EQ( dl.GetIndices().size(), 3u );
    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].Texture, nullptr );
}

// ==================================================================================================
// THE RENDER TRANSFORM (Ю8)
//
// The whole design decision of that task is asserted by the first test below and by
// `ATransformDoesNotBreakTheBatch`: a transform is applied to the POSITIONS as they are emitted, not
// carried as GPU state. Everything else follows from those two — no draw call is added, the batch key
// is untouched, and a canvas that transforms nothing emits the bytes it always emitted.
// ==================================================================================================

// The invariant the task's byte-identity rests on, asserted at the only place it can be: with nothing
// pushed, a position is STORED, not transformed. EXPECT_EQ on floats is deliberate — "near" would pass
// for a value that had been through a matrix, which is exactly what must not have happened.
TEST( DrawList2D, WithNoTransformPushedPositionsAreStoredVerbatim )
{
    DrawList2D dl;
    EXPECT_FALSE( dl.HasTransform() );

    // Values chosen so a round trip through any multiply would show: none is exactly representable as
    // a short binary fraction.
    dl.AddRectFilled( { 0.1f, 0.3f }, { 100.7f, 70.9f }, { 1, 1, 1, 1 } );
    const auto& v = dl.GetVertices();
    ASSERT_EQ( v.size(), 4u );
    EXPECT_EQ( v[0].Position.x, 0.1f );
    EXPECT_EQ( v[0].Position.y, 0.3f );
    EXPECT_EQ( v[2].Position.x, 100.7f );
    EXPECT_EQ( v[2].Position.y, 70.9f );
}

// A 90-degree turn is the one rotation whose result can be written down exactly, so it is the one that
// can be asserted rather than approximated. Positive = clockwise in this y-down space.
TEST( DrawList2D, AQuarterTurnClockwiseMovesTheCornersWhereItShould )
{
    DrawList2D dl;
    // A 100x40 rect at the origin, turned about its own centre (50,20).
    dl.PushTransform( R2D::MakeTransform2D( { 50.0f, 20.0f }, 90.0f, { 1.0f, 1.0f } ) );
    EXPECT_TRUE( dl.HasTransform() );
    dl.AddRectFilled( { 0.0f, 0.0f }, { 100.0f, 40.0f }, { 1, 1, 1, 1 } );
    dl.PopTransform();
    EXPECT_FALSE( dl.HasTransform() );

    const auto& v = dl.GetVertices();
    ASSERT_EQ( v.size(), 4u );
    // Clockwise by 90 about (50,20): the top-left corner (0,0) goes to (70,-30).
    EXPECT_NEAR( v[0].Position.x, 70.0f, 1e-3f );
    EXPECT_NEAR( v[0].Position.y, -30.0f, 1e-3f );
    // ...and the bottom-right corner (100,40) to (30,70).
    EXPECT_NEAR( v[2].Position.x, 30.0f, 1e-3f );
    EXPECT_NEAR( v[2].Position.y, 70.0f, 1e-3f );

    // Popped: the next primitive is stored verbatim again.
    dl.AddRectFilled( { 5.0f, 6.0f }, { 7.0f, 8.0f }, { 1, 1, 1, 1 } );
    EXPECT_EQ( dl.GetVertices()[4].Position.x, 5.0f );
    EXPECT_EQ( dl.GetVertices()[4].Position.y, 6.0f );
}

// THE MEASUREMENT BEHIND THE DESIGN DECISION. Batches break on texture, text mode and clip
// (CurrentCommand), and a transform is none of those. Two rects that would have shared a command still
// share it when one of them is turned — which is what a per-batch matrix could not have given, because
// a push constant is per draw.
TEST( DrawList2D, ATransformDoesNotBreakTheBatch )
{
    DrawList2D dl;
    dl.AddRectFilled( { 0, 0 }, { 10, 10 }, { 1, 1, 1, 1 } );
    dl.PushTransform( R2D::MakeTransform2D( { 5.0f, 5.0f }, 30.0f, { 2.0f, 0.5f } ) );
    dl.AddRectFilled( { 20, 0 }, { 30, 10 }, { 1, 1, 1, 1 } );
    dl.PopTransform();
    dl.AddRectFilled( { 40, 0 }, { 50, 10 }, { 1, 1, 1, 1 } );

    ASSERT_EQ( dl.GetCommands().size(), 1u ) << "a transform opened a draw call of its own";
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, 18u );
}

// Nesting is what makes a parent carry its children, and the ORDER is the load-bearing half: the outer
// matrix must be applied AFTER the inner one. Asserted with two transforms that do not commute — a
// translation and a scale.
TEST( DrawList2D, NestedTransformsComposeOuterAfterInner )
{
    // Outer: scale x2 about the origin. Inner: scale x1 about a pivot 100 to the right, i.e. a pure
    // rotation by 180 there, which lands the origin at 200.
    const glm::mat3 outer = R2D::MakeTransform2D( { 0.0f, 0.0f }, 0.0f, { 2.0f, 2.0f } );
    const glm::mat3 inner = R2D::MakeTransform2D( { 100.0f, 0.0f }, 180.0f, { 1.0f, 1.0f } );

    DrawList2D dl;
    dl.PushTransform( outer );
    dl.PushTransform( inner );
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1, 1, 1, 1 } );
    dl.PopTransform();
    dl.PopTransform();

    // inner: (0,0) -> (200,0). outer: (200,0) -> (400,0). Composed the other way round the outer scale
    // would run first and the answer would be 200 — which is why this pair was chosen: they do not
    // commute, so the assertion is about the ORDER and not merely about both being applied.
    EXPECT_NEAR( dl.GetVertices()[0].Position.x, 400.0f, 1e-2f );
    EXPECT_NEAR( dl.GetVertices()[0].Position.y, 0.0f, 1e-2f );

    // One pop is one level: after the inner pop the outer must still be in force.
    dl.PushTransform( outer );
    dl.PushTransform( inner );
    dl.PopTransform();
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1, 1, 1, 1 } );
    EXPECT_NEAR( dl.GetVertices()[4].Position.x, 0.0f, 1e-2f );
    dl.PopTransform();
    EXPECT_FALSE( dl.HasTransform() );
}

// The BOX half of a rotated clip: a scissor is all the hardware has and it is axis-aligned, so the command
// still carries the box around the clipper. What changed in Ю9 is that the box is no longer the WHOLE clip.
TEST( DrawList2D, AClipUnderATransformIsStoredAsItsScreenBoundingBox )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 50.0f, 50.0f }, 45.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
    dl.AddRectFilled( { 0, 0 }, { 100, 100 }, { 1, 1, 1, 1 } );
    dl.PopClipRect();
    dl.PopTransform();

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    const glm::vec4 clip = dl.GetCommands()[0].ClipRect;
    // A 100x100 square turned 45 degrees about its centre spans 100*sqrt(2) each way, still centred
    // on (50,50).
    const float diag = 100.0f * std::sqrt( 2.0f );
    EXPECT_NEAR( clip.z, diag, 1e-2f );
    EXPECT_NEAR( clip.w, diag, 1e-2f );
    EXPECT_NEAR( clip.x, 50.0f - diag * 0.5f, 1e-2f );
    EXPECT_NEAR( clip.y, 50.0f - diag * 0.5f, 1e-2f );

    // Untransformed, the same call stores the rect itself — the neutral case is not merely close to the
    // old behaviour, it IS it.
    DrawList2D plain;
    plain.PushClipRect( { 3.5f, 4.5f }, { 13.5f, 24.5f } );
    plain.AddRectFilled( { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    ASSERT_EQ( plain.GetCommands().size(), 1u );
    EXPECT_EQ( plain.GetCommands()[0].ClipRect.x, 3.5f );
    EXPECT_EQ( plain.GetCommands()[0].ClipRect.y, 4.5f );
    EXPECT_EQ( plain.GetCommands()[0].ClipRect.z, 10.0f );
    EXPECT_EQ( plain.GetCommands()[0].ClipRect.w, 20.0f );
}

// Glass is the ONE primitive a moved vertex cannot transform, because its mask is an SDF the fragment
// shader evaluates over screen positions. So the command carries the way BACK from the screen, and its
// rect stays in its own space. Untransformed that inverse is the identity and the feather is one.
TEST( DrawList2D, GlassCarriesTheWayBackFromTheScreen )
{
    DrawList2D plain;
    plain.AddGlassRect( { 10.0f, 20.0f }, { 110.0f, 70.0f }, { 1, 1, 1, 0.5f }, 8.0f, 1.0f );
    ASSERT_EQ( plain.GetCommands().size(), 1u );
    EXPECT_TRUE( R2D::IsIdentity2D( plain.GetCommands()[0].GlassInverse ) );
    EXPECT_EQ( plain.GetCommands()[0].GlassFeather, 1.0f );
    EXPECT_EQ( plain.GetCommands()[0].GlassRect.x, 10.0f );

    DrawList2D dl;
    // Scaled by 4 in both axes: one screen pixel is a quarter of a pixel in the rect's own space.
    dl.PushTransform( R2D::MakeTransform2D( { 0.0f, 0.0f }, 0.0f, { 4.0f, 4.0f } ) );
    dl.AddGlassRect( { 10.0f, 20.0f }, { 110.0f, 70.0f }, { 1, 1, 1, 0.5f }, 8.0f, 1.0f );
    dl.PopTransform();

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    const auto& cmd = dl.GetCommands()[0];
    EXPECT_NEAR( cmd.GlassFeather, 0.25f, 1e-4f );
    // The rect is unchanged (own space) while the vertices moved (screen space) — that pair IS the fix.
    EXPECT_EQ( cmd.GlassRect.x, 10.0f );
    EXPECT_NEAR( dl.GetVertices()[0].Position.x, 40.0f, 1e-3f );
    // And the inverse really is the inverse: a screen point maps back onto the vertex it came from.
    const glm::vec2 back = R2D::TransformPoint2D( cmd.GlassInverse, { 40.0f, 80.0f } );
    EXPECT_NEAR( back.x, 10.0f, 1e-3f );
    EXPECT_NEAR( back.y, 20.0f, 1e-3f );
}

// Reset is per frame, so a transform left pushed by a walk that returned early must not reach the next
// frame's first vertex.
TEST( DrawList2D, ResetDropsAPushedTransform )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 0.0f, 0.0f }, 0.0f, { 3.0f, 3.0f } ) );
    dl.Reset();
    EXPECT_FALSE( dl.HasTransform() );
    dl.AddRectFilled( { 7.0f, 9.0f }, { 8.0f, 10.0f }, { 1, 1, 1, 1 } );
    EXPECT_EQ( dl.GetVertices()[0].Position.x, 7.0f );
}

// A scale of zero on an axis is authorable, and the inverse of that transform does not exist. What must
// not happen is an infinity travelling into a pointer position.
TEST( DrawList2D, ADegenerateTransformInvertsToSomethingFinite )
{
    const glm::mat3 flat = R2D::MakeTransform2D( { 40.0f, 60.0f }, 0.0f, { 0.0f, 1.0f } );
    const glm::vec2 p    = R2D::TransformPoint2D( R2D::InverseTransform2D( flat ), { 1234.0f, 5678.0f } );
    EXPECT_TRUE( std::isfinite( p.x ) ) << "an infinity from a singular inverse reached a pointer position";
    EXPECT_TRUE( std::isfinite( p.y ) ) << "an infinity from a singular inverse reached a pointer position";
    EXPECT_EQ( R2D::MeanScale2D( flat ), 0.0f );
}

// ==================================================================================================
// Ю9 — EXACT CLIPPING BY A ROTATED CLIPPER. The box above is a superset of the clipper; the four oblique
// edges are the rest of it, and they cut the geometry here rather than being handed to a scissor that
// cannot express them. Everything below is about that cut, and about the cases where it must NOT happen.
// ==================================================================================================

namespace
{
    // Is `p` inside the region, allowing for the fact that a cut vertex lands exactly ON a plane?
    bool InsideRegion( const R2D::ClipRegion2D& r, const glm::vec2& p, float slack )
    {
        for ( uint32_t i = 0; i < r.PlaneCount; ++i )
            if ( R2D::ClipPlaneDistance( r.Planes[i], p ) < -slack )
                return false;
        return true;
    }
} // namespace

// THE INVARIANT THE WHOLE FEATURE RESTS ON, and the one that separates it from Ю8: after a rotated clipper,
// no emitted vertex lies outside the clipper's own quadrilateral. Written as "every vertex" rather than as
// a count, because a count would pass for a clipper that cut the wrong corner.
TEST( DrawList2D, ARotatedClipperCutsTheGeometryAndNotOnlyTheScissorBox )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 50.0f, 50.0f }, 45.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
    ASSERT_EQ( dl.GetClipRegion().PlaneCount, 4u ) << "a 45-degree clipper produced no oblique constraint";

    // A rectangle covering the clipper's whole SCREEN BOUNDING BOX. Under Ю8 every corner of it survived,
    // because the box was the entire clip; now the four corners of the box are outside the diamond.
    const float diag = 100.0f * std::sqrt( 2.0f );
    dl.PushTransform( R2D::MakeTransform2D( { 0.0f, 0.0f }, 0.0f, { 1.0f, 1.0f } ) ); // straight, inside the turn
    dl.PopTransform();
    dl.AddRectFilled( { -100.0f, -100.0f }, { 200.0f, 200.0f }, { 1, 1, 1, 1 } );
    dl.PopClipRect();
    dl.PopTransform();

    ASSERT_FALSE( dl.GetVertices().empty() ) << "the clipper removed everything, so nothing is asserted";
    DrawList2D probe;
    probe.PushTransform( R2D::MakeTransform2D( { 50.0f, 50.0f }, 45.0f, { 1.0f, 1.0f } ) );
    probe.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
    const R2D::ClipRegion2D region = probe.GetClipRegion();

    for ( const auto& v : dl.GetVertices() )
        EXPECT_TRUE( InsideRegion( region, v.Position, 1e-2f ) )
             << "a vertex at (" << v.Position.x << "," << v.Position.y << ") survived outside the clipper";

    // Not vacuous: the box the scissor gets is strictly bigger than the diamond, so something WAS removed.
    EXPECT_NEAR( dl.GetCommands()[0].ClipRect.z, diag, 1e-2f );
    EXPECT_LT( dl.GetVertices()[0].Position.x, 1e6f ); // finite, i.e. no infinity walked out of a split
}

// THE NEUTRAL CASES, and they are what "byte-identical" rests on. A clipper that is not turned — untouched,
// purely scaled, or turned by a quarter or a half — IS its own box, so it must carry no plane and cut no
// triangle: same four shared corners, same six indices, same bytes as before any of this existed. The
// quarter and half turns are in the list because cos(pi/2) is -4.37e-8 rather than 0 in float, so a test
// written against the matrix would have sent a square clipper down the cutting path.
TEST( DrawList2D, AnUnrotatedClipperCarriesNoObliquePlaneAtAll )
{
    const glm::mat3 cases[] = { glm::mat3( 1.0f ), R2D::MakeTransform2D( { 7.0f, 9.0f }, 0.0f, { 3.0f, 0.5f } ),
                                R2D::MakeTransform2D( { 7.0f, 9.0f }, 90.0f, { 1.0f, 1.0f } ),
                                R2D::MakeTransform2D( { 7.0f, 9.0f }, 180.0f, { 1.0f, 1.0f } ) };
    for ( const glm::mat3& m : cases )
    {
        DrawList2D dl;
        dl.PushTransform( m );
        dl.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
        dl.AddRectFilled( { 10.0f, 20.0f }, { 40.0f, 50.0f }, { 1, 1, 1, 1 } );
        EXPECT_EQ( dl.GetClipRegion().PlaneCount, 0u ) << "an axis-aligned clipper produced a plane";
        // Four shared corners and six indices: the clipper did not touch the geometry.
        EXPECT_EQ( dl.GetVertices().size(), 4u );
        EXPECT_EQ( dl.GetIndices().size(), 6u );
    }

    // And with no clip pushed at all, the position is still STORED rather than tested against anything.
    DrawList2D bare;
    bare.AddRectFilled( { 0.1f, 0.3f }, { 100.7f, 70.9f }, { 1, 1, 1, 1 } );
    EXPECT_EQ( bare.GetVertices()[0].Position.x, 0.1f );
    EXPECT_EQ( bare.GetClipRegion().PlaneCount, 0u );

    // THE OTHER HALF OF THE SAME DECISION, without which the tolerance above is a hole rather than a rule:
    // a turn small enough to see on a 300 px panel — a hundredth of a degree is 0.05 px at the corner —
    // is oblique and IS cut. Only turns whose departure from the box is under a 256th of a pixel are free.
    DrawList2D barely;
    barely.PushTransform( R2D::MakeTransform2D( { 150.0f, 150.0f }, 0.01f, { 1.0f, 1.0f } ) );
    barely.PushClipRect( { 0.0f, 0.0f }, { 300.0f, 300.0f } );
    EXPECT_EQ( barely.GetClipRegion().PlaneCount, 4u )
         << "a rotation visible on screen was rounded down to its bounding box";
}

// TWO PATHS THAT MUST AGREE, pinned rather than trusted: the clipping emitter and the verbatim one are
// different code, and a primitive that lies entirely INSIDE a rotated clipper must come out of the first
// exactly as it comes out of the second. Without this the clipper could be quietly wrong everywhere and
// only the cut cases would show it.
TEST( DrawList2D, AClipperThatCutsNothingEmitsWhatTheUnclippedPathEmits )
{
    const auto build = []( bool withClipper )
    {
        DrawList2D dl;
        dl.PushTransform( R2D::MakeTransform2D( { 500.0f, 500.0f }, 33.0f, { 1.0f, 1.0f } ) );
        if ( withClipper )
            dl.PushClipRect( { -1000.0f, -1000.0f }, { 2000.0f, 2000.0f } ); // far outside everything below
        dl.AddRectFilled( { 100.0f, 100.0f }, { 260.0f, 180.0f }, { 0.2f, 0.4f, 0.6f, 1.0f } );
        dl.AddRectFilled( { 100.0f, 200.0f }, { 260.0f, 280.0f }, { 1, 1, 1, 1 }, 12.0f );  // rounded: a fan
        dl.AddRing( { 400.0f, 400.0f }, 40.0f, 20.0f, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, 12 ); // a strip
        dl.AddTriangleFilled( { 600, 600 }, { 640, 600 }, { 620, 640 }, { 1, 1, 1, 1 } );
        dl.AddLine( { 700, 700 }, { 780, 740 }, { 1, 1, 1, 1 }, 3.0f );
        return dl;
    };

    const DrawList2D plain   = build( false );
    const DrawList2D clipped = build( true );
    ASSERT_GT( clipped.GetClipRegion().PlaneCount, 0u ) << "the clipper was pruned away, so nothing is proven";

    // The clipped path de-indexes (a cut triangle owns its corners), so the buffers cannot be compared byte
    // for byte. What must match is the PICTURE: the same triangles, corner for corner.
    const auto triangles = []( const DrawList2D& dl )
    {
        std::vector<std::array<glm::vec2, 3>> out;
        const auto&                           v = dl.GetVertices();
        const auto&                           i = dl.GetIndices();
        for ( std::size_t t = 0; t + 2 < i.size(); t += 3 )
            out.push_back( { v[i[t]].Position, v[i[t + 1]].Position, v[i[t + 2]].Position } );
        return out;
    };
    const auto a = triangles( plain );
    const auto b = triangles( clipped );
    ASSERT_EQ( a.size(), b.size() ) << "the clipper added or dropped triangles where it should cut nothing";
    for ( std::size_t t = 0; t < a.size(); ++t )
        for ( int c = 0; c < 3; ++c )
        {
            EXPECT_NEAR( a[t][c].x, b[t][c].x, 1e-3f ) << "triangle " << t << " corner " << c;
            EXPECT_NEAR( a[t][c].y, b[t][c].y, 1e-3f ) << "triangle " << t << " corner " << c;
        }
}

// NESTING, AND THE TRAP Ю8 NAMED. Two rotated clippers whose quadrilaterals overlap only in part: what
// survives must be inside BOTH. A composition that replaced the outer constraint with the inner one — or
// that kept only the outer — leaves a point that this test names explicitly.
TEST( DrawList2D, TwoRotatedClippersIntersectRatherThanReplace )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 200.0f, 200.0f }, 30.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 100.0f, 100.0f }, { 300.0f, 300.0f } ); // outer diamond
    const R2D::ClipRegion2D outerOnly = dl.GetClipRegion();

    dl.PushTransform( R2D::MakeTransform2D( { 260.0f, 200.0f }, -50.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 160.0f, 120.0f }, { 360.0f, 280.0f } ); // inner, turned the other way and offset
    const R2D::ClipRegion2D both = dl.GetClipRegion();
    dl.PopClipRect();
    dl.PopTransform();

    DrawList2D innerAlone;
    innerAlone.PushTransform( R2D::MakeTransform2D( { 200.0f, 200.0f }, 30.0f, { 1.0f, 1.0f } ) );
    innerAlone.PushTransform( R2D::MakeTransform2D( { 260.0f, 200.0f }, -50.0f, { 1.0f, 1.0f } ) );
    innerAlone.PushClipRect( { 160.0f, 120.0f }, { 360.0f, 280.0f } );
    const R2D::ClipRegion2D innerOnly = innerAlone.GetClipRegion();

    // A point in the inner quad and NOT in the outer one: kept by "replace", refused by "intersect".
    int inInnerNotOuter = 0, inOuterNotInner = 0, inBoth = 0;
    for ( float y = 60.0f; y < 420.0f; y += 3.0f )
        for ( float x = 60.0f; x < 420.0f; x += 3.0f )
        {
            const glm::vec2 p( x, y );
            const bool      o = R2D::ClipRegionContains( outerOnly, p );
            const bool      i = R2D::ClipRegionContains( innerOnly, p );
            const bool      c = R2D::ClipRegionContains( both, p );
            EXPECT_EQ( c, o && i ) << "at (" << x << "," << y << ") the nested clip is not the intersection";
            inInnerNotOuter += ( i && !o ) ? 1 : 0;
            inOuterNotInner += ( o && !i ) ? 1 : 0;
            inBoth += ( o && i ) ? 1 : 0;
        }
    // The three counts together are what make the assertion above non-vacuous IN BOTH DIRECTIONS: there is
    // a region only the outer refuses, a region only the inner refuses, and a region both accept.
    EXPECT_GT( inInnerNotOuter, 50 ) << "the two clippers nearly coincide; replacement would be invisible";
    EXPECT_GT( inOuterNotInner, 50 );
    EXPECT_GT( inBoth, 50 );

    dl.PopClipRect();
    dl.PopTransform();
}

// A NESTED CLIP CAN BE EMPTY, and an empty clip used to be indistinguishable from NO clip: the intersection
// stored a width of zero and the backend reads `w <= 0` as "unclipped", so two disjoint clippers drew over
// the WHOLE viewport while the pointer was refused everywhere. One picture, two answers.
TEST( DrawList2D, TwoDisjointClippersDrawNothingRatherThanEverything )
{
    DrawList2D dl;
    dl.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
    dl.PushClipRect( { 400.0f, 400.0f }, { 500.0f, 500.0f } ); // no overlap at all
    dl.AddRectFilled( { 0.0f, 0.0f }, { 1000.0f, 1000.0f }, { 1, 1, 1, 1 } );
    dl.AddText( reinterpret_cast<const void*>( 0x1 ), { 0, 0 }, { 10, 10 }, { 0, 0 }, { 1, 1 }, { 1, 1, 1, 1 } );
    dl.AddGlassRect( { 0, 0 }, { 50, 50 }, { 1, 1, 1, 0.5f } );
    dl.PopClipRect();
    dl.PopClipRect();

    EXPECT_TRUE( dl.Empty() ) << "geometry survived a clip region with no area in it";
    EXPECT_TRUE( dl.GetCommands().empty() ) << "a command was opened for a clip that keeps nothing";

    // And the pointer says the same thing about the same region.
    DrawList2D probe;
    probe.PushClipRect( { 0.0f, 0.0f }, { 100.0f, 100.0f } );
    probe.PushClipRect( { 400.0f, 400.0f }, { 500.0f, 500.0f } );
    EXPECT_FALSE( R2D::ClipRegionContains( probe.GetClipRegion(), { 50.0f, 50.0f } ) );
    EXPECT_FALSE( R2D::ClipRegionContains( probe.GetClipRegion(), { 450.0f, 450.0f } ) );
}

// GLASS IS CUT TOO. Its mask is an SDF over gl_FragCoord and its push-constant block is full at 128 bytes,
// so a fragment-side clip could not have reached it; cutting the geometry does, with no state at all.
TEST( DrawList2D, GlassIsCutByARotatedClipperLikeEverythingElse )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 100.0f, 100.0f }, 45.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 50.0f, 50.0f }, { 150.0f, 150.0f } );
    dl.AddGlassRect( { 0.0f, 0.0f }, { 200.0f, 200.0f }, { 1, 1, 1, 0.5f }, 8.0f, 1.0f );
    const R2D::ClipRegion2D region = dl.GetClipRegion();
    dl.PopClipRect();
    dl.PopTransform();

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_TRUE( dl.GetCommands()[0].Glass );
    EXPECT_GT( dl.GetCommands()[0].IndexCount, 0u );
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, dl.GetIndices().size() )
         << "the glass command's index count no longer describes what was emitted for it";
    for ( const auto& v : dl.GetVertices() )
        EXPECT_TRUE( InsideRegion( region, v.Position, 1e-2f ) ) << "a glass corner survived outside the clipper";

    // Entirely outside: no command at all rather than a command drawing nothing.
    DrawList2D away;
    away.PushTransform( R2D::MakeTransform2D( { 100.0f, 100.0f }, 45.0f, { 1.0f, 1.0f } ) );
    away.PushClipRect( { 50.0f, 50.0f }, { 150.0f, 150.0f } );
    away.AddGlassRect( { 400.0f, 400.0f }, { 500.0f, 500.0f }, { 1, 1, 1, 0.5f } );
    EXPECT_TRUE( away.GetCommands().empty() );
}

// THE BUDGET, AND WHAT HAPPENS PAST IT. Sixteen half-planes is four rotated clippers; a fifth cannot be
// stored, and what must then hold is that the region stays a SUPERSET of the exact intersection — never
// tighter — so the picture and the pointer still agree, about a slightly looser region, and the refusal is
// reported rather than assumed.
TEST( DrawList2D, PastThePlaneBudgetTheRegionStaysASupersetAndSaysSo )
{
    R2D::ClipRegion2D region;
    bool              exact = true;
    for ( int i = 0; i < 6; ++i )
    {
        const glm::mat3 m = R2D::MakeTransform2D( { 300.0f, 300.0f }, 7.0f + 11.0f * i, { 1.0f, 1.0f } );
        exact             = R2D::IntersectClipRegion( region, m, { 100.0f, 100.0f }, { 500.0f, 500.0f } ) && exact;
    }
    EXPECT_FALSE( exact ) << "six rotated clippers fitted in a sixteen-plane budget without saying so";
    EXPECT_LE( region.PlaneCount, R2D::kMaxClipPlanes );

    // Superset: every point the exact intersection keeps, this region keeps too.
    int kept = 0;
    for ( float y = 90.0f; y < 510.0f; y += 4.0f )
        for ( float x = 90.0f; x < 510.0f; x += 4.0f )
        {
            bool inAll = true;
            for ( int i = 0; i < 6 && inAll; ++i )
            {
                R2D::ClipRegion2D one;
                const glm::mat3   m = R2D::MakeTransform2D( { 300.0f, 300.0f }, 7.0f + 11.0f * i, { 1.0f, 1.0f } );
                (void)R2D::IntersectClipRegion( one, m, { 100.0f, 100.0f }, { 500.0f, 500.0f } );
                inAll = R2D::ClipRegionContains( one, { x, y } );
            }
            if ( inAll )
            {
                ++kept;
                EXPECT_TRUE( R2D::ClipRegionContains( region, { x, y } ) )
                     << "the overflowing region refused a point the exact intersection keeps";
            }
        }
    EXPECT_GT( kept, 100 ) << "the exact intersection was empty, so the superset claim is vacuous";
}

// A ROTATED CLIP COSTS NO DRAW CALL, which is the measurement behind choosing the CPU over a stencil or a
// push-constant mask: the oblique half never reaches the GPU, so it is not in the batch key. Two rects that
// shared a command still share it when a turned clipper cuts them both.
TEST( DrawList2D, AnObliqueClipDoesNotOpenADrawCallOfItsOwn )
{
    DrawList2D dl;
    dl.PushTransform( R2D::MakeTransform2D( { 100.0f, 100.0f }, 20.0f, { 1.0f, 1.0f } ) );
    dl.PushClipRect( { 0.0f, 0.0f }, { 200.0f, 200.0f } );
    dl.AddRectFilled( { 10, 10 }, { 90, 90 }, { 1, 1, 1, 1 } );
    dl.AddRectFilled( { 110, 10 }, { 190, 90 }, { 1, 1, 1, 1 } );
    // Read while the clip is still in force: after the pops it is the ground state and says nothing.
    ASSERT_EQ( dl.GetClipRegion().PlaneCount, 4u ) << "the clip was not oblique, so nothing is proven";
    dl.PopClipRect();
    dl.PopTransform();

    EXPECT_EQ( dl.GetCommands().size(), 1u ) << "an oblique clip opened a draw call of its own";
}
// --- Ю11: a UI-domain material is a FILL, and what it costs the batcher ------------------------------
//
// The whole design rests on one claim — a material adds nothing to the batch key that a texture does not
// already add, and adds NOTHING AT ALL to an element that has none. These assert both halves of that,
// plus the refusal that keeps a null from becoming an ordinary white rect.

namespace
{
    // Two distinct addresses standing in for two resolved UIMaterialCache::Entry values. The list treats
    // them as opaque ids and never dereferences them, which is exactly why a test needs no GPU.
    const char kMaterialA = 0;
    const char kMaterialB = 0;
} // namespace

TEST( DrawList2DMaterial, AMaterialRectEmitsOneQuadCarryingItsMaterial )
{
    DrawList2D dl;
    dl.AddMaterialRect( &kMaterialA, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].Material, &kMaterialA );
    EXPECT_EQ( dl.GetCommands()[0].Texture, nullptr );
    EXPECT_FALSE( dl.GetCommands()[0].Text );
    EXPECT_EQ( dl.GetIndices().size(), 6u );
    ASSERT_EQ( dl.GetVertices().size(), 4u );
    // UVs span the element's own rect, which is the whole contract a material fragment is written
    // against (Common/UIVertex.glslh). A material owns its shape through that UV, so there is no
    // rounding argument to check.
    EXPECT_NEAR( dl.GetVertices()[0].UV.x, 0.0f, kEps );
    EXPECT_NEAR( dl.GetVertices()[2].UV.x, 1.0f, kEps );
    EXPECT_NEAR( dl.GetVertices()[2].UV.y, 1.0f, kEps );
}

TEST( DrawList2DMaterial, TwoElementsOnOneMaterialAreONEBatch )
{
    // The measured claim from UI_MaterialProbe, as an assertion: one runtime material per ASSET, not per
    // element, so two panels pointing at the same `.demat` cost one draw call between them. UE's
    // GetDynamicMaterial() is the counter-example — it silently gives each widget its own instance, and
    // their own documentation names that as the dominant real-world batching cost of UI materials.
    DrawList2D dl;
    dl.AddMaterialRect( &kMaterialA, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddMaterialRect( &kMaterialA, { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    ASSERT_EQ( dl.GetCommands().size(), 1u );
    EXPECT_EQ( dl.GetCommands()[0].IndexCount, 12u );
}

TEST( DrawList2DMaterial, ADifferentMaterialOpensItsOwnBatch )
{
    DrawList2D dl;
    dl.AddMaterialRect( &kMaterialA, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddMaterialRect( &kMaterialB, { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    ASSERT_EQ( dl.GetCommands().size(), 2u );
    EXPECT_EQ( dl.GetCommands()[0].Material, &kMaterialA );
    EXPECT_EQ( dl.GetCommands()[1].Material, &kMaterialB );
}

TEST( DrawList2DMaterial, AMaterialQuadNeverMergesWithAPlainOne )
{
    // Both are "solid" by every other test in the key — null texture, not text, same clip — so without
    // the material in the merge test the second quad would be drawn by the FIRST one's pipeline. That is
    // the failure this asserts against, in both directions.
    DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddMaterialRect( &kMaterialA, { 20.0f, 0.0f }, { 30.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );
    dl.AddRectFilled( { 40.0f, 0.0f }, { 50.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    ASSERT_EQ( dl.GetCommands().size(), 3u );
    EXPECT_EQ( dl.GetCommands()[0].Material, nullptr );
    EXPECT_EQ( dl.GetCommands()[1].Material, &kMaterialA );
    EXPECT_EQ( dl.GetCommands()[2].Material, nullptr );
}

TEST( DrawList2DMaterial, ACanvasWithNoMaterialRecordsExACTLYWhatItAlwaysDid )
{
    // THE INVARIANT OF THIS WHOLE LINE, at the level a test can hold it: nothing that does not ask for a
    // material may differ by one byte because materials exist. The frame half of the same claim is
    // measured on UI_ElementProbe — 0 differing pixels of 560 560, floor 0 over three repeats.
    DrawList2D dl;
    dl.AddRectFilled( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 0.5f, 0.25f, 1.0f }, 4.0f );
    dl.AddRect( { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, 2.0f );
    dl.AddText( &kMaterialA, { 1.0f, 1.0f }, { 5.0f, 5.0f }, { 0.0f, 0.0f }, { 1.0f, 1.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f } );

    for ( const auto& cmd : dl.GetCommands() )
        EXPECT_EQ( cmd.Material, nullptr ) << "a primitive that names no material must not carry one";
}

TEST( DrawList2DMaterial, ANullMaterialRecordsNOTHINGRatherThanAWhiteRect )
{
    // AddQuad's null texture means "the backend's 1x1 white", so a null material falling through to it
    // would draw the element's authored colour and look ALMOST right — the silent wrong answer §1.4
    // forbids. Resolving a handle is the caller's job precisely because only the caller knows which
    // handle failed, and UIMaterialCache::Resolve never answers null for a set one.
    DrawList2D dl;
    dl.AddMaterialRect( nullptr, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } );

    EXPECT_TRUE( dl.Empty() );
    EXPECT_TRUE( dl.GetCommands().empty() );
}

// RDG-FAULT1: a UI material whose parameter row does not fit its shader's parameter layout - empty, SHORT, or with
// another field at a slot - falls back to the default UI material for ITS draws only - the others draw as
// authored, every draw still declares a block (the UI node is not faulted), and the reason is reported once across
// frames, not per draw or per frame.
TEST( UIMaterialFallback, OneBrokenMaterialFallsBackAloneAndIsReportedOnce )
{
    using R2D::UIMaterialFallback;
    using Fields = std::vector<std::string>;
    struct Draw
    {
        std::string Name;
        bool        RowBlock;
        Fields      Layout; // the recording shader's parameter layout
        Fields      Row;    // the fields of the schema the row was built from
        std::size_t Slots;  // the vec4 slots the row holds
    };
    const Fields            gradient = { "TopColor", "BottomColor" };
    const std::vector<Draw> canvas   = { { "UIGradient", true, gradient, gradient, 2 },
                                         { "UIBroken", true, gradient, {}, 0 },
                                         { "UIPlainTint", false, {}, {}, 0 },
                                         { "UIBroken", true, gradient, {}, 0 },
                                         { "UIShort", true, gradient, { "TopColor" }, 1 },
                                         { "UIRenamed", true, gradient, { "TopColor", "Bottom" }, 2 } };
    UIMaterialFallback      fallback;
    int                     reports = 0;
    for ( int frame = 0; frame < 2; ++frame )
    {
        std::vector<std::string> declared;
        for ( const Draw& draw : canvas )
        {
            const std::string fault =
                 UIMaterialFallback::RowFault( draw.RowBlock, draw.Layout, draw.Row, draw.Slots );
            const UIMaterialFallback::Verdict verdict = fallback.Admit( draw.Name, fault );
            if ( verdict == UIMaterialFallback::Verdict::DefaultFirstReport )
                ++reports;
            declared.push_back( verdict == UIMaterialFallback::Verdict::Draws ? draw.Name
                                                                              : std::string( "default" ) );
        }
        ASSERT_EQ( declared.size(), canvas.size() ) << "every draw declares a block, the broken one included";
        EXPECT_EQ( declared[0], "UIGradient" );
        EXPECT_EQ( declared[1], "default" );
        EXPECT_EQ( declared[2], "UIPlainTint" ) << "a material without a row block needs no row";
        EXPECT_EQ( declared[3], "default" );
        EXPECT_EQ( declared[4], "default" ) << "a SHORT row is not a row of this shader";
        EXPECT_EQ( declared[5], "default" ) << "a row whose slot 1 is another field is not a row of this shader";
    }
    EXPECT_EQ( reports, 3 ) << "each broken material is logged once across frames";
    EXPECT_EQ( fallback.ReportedCount(), 3u );
    EXPECT_NE( UIMaterialFallback::RowFault( true, gradient, Fields{ "TopColor" }, 1 ).find( "holds 1 slot(s)" ),
               std::string::npos )
         << "the short row's reason names its size";
    EXPECT_NE(
         UIMaterialFallback::RowFault( true, gradient, Fields{ "TopColor", "Bottom" }, 2 ).find( "'BottomColor'" ),
         std::string::npos )
         << "the missing field is named";
    // Many materials share a shader: the report names the material ASSET first, then the shader.
    const std::string report = UIMaterialFallback::Report( "UI_Broken", "UIGradient", "short row", "UIMatError" );
    EXPECT_NE( report.find( "material 'UI_Broken' (shader 'UIGradient')" ), std::string::npos ) << report;
    EXPECT_NE( report.find( "'UIMatError'" ), std::string::npos ) << report;
}

// RDG-FAULT1 C3b: a UI draw is prepared ONCE per frame. Render2D's setup (DeclareInto) prepares every command of
// the draw list through PreparedDraws - for a UI material that is UIMaterialCache::DrawableOrDefault's
// PrepareDraw: row, push, validation, or the default material - and Flush records PreparedDraws::Draws() as they
// are (census RenderGraphCompile.UIMaterialDrawsFallBackPerDrawNotPerNode: Flush resolves nothing). Here: the
// preparation runs exactly once per command, the exec walk sees each drawn command once with its prepared value
// and its index back into the list, and a list nobody prepared is not Ready (Flush refuses it instead of preparing
// it itself).
TEST( PreparedDraws, EveryDrawIsPreparedOnceAndTheExecRecordsThePreparedList )
{
    struct Command
    {
        int  Id;
        bool Draws;
    };
    const std::vector<Command> commands = { { 0, true }, { 1, false }, { 2, true }, { 3, true } };
    R2D::PreparedDraws<int>    prepared;
    EXPECT_FALSE( prepared.Ready() ) << "a list nobody prepared must not be recorded";
    std::vector<int> preparedIds;
    prepared.Prepare( commands,
                      [&]( const Command& command ) -> std::optional<int>
                      {
                          preparedIds.push_back( command.Id );
                          if ( !command.Draws )
                          {
                              return std::nullopt;
                          }
                          return command.Id * 10;
                      } );
    EXPECT_EQ( preparedIds, ( std::vector<int>{ 0, 1, 2, 3 } ) ) << "each command is prepared exactly once";
    ASSERT_TRUE( prepared.Ready() );
    std::vector<uint32_t> recorded;
    std::vector<int>      values;
    for ( const auto& draw : prepared.Draws() )
    {
        recorded.push_back( draw.Command );
        values.push_back( draw.Value );
    }
    EXPECT_EQ( recorded, ( std::vector<uint32_t>{ 0, 2, 3 } ) ) << "a skipped command opens no block";
    EXPECT_EQ( values, ( std::vector<int>{ 0, 20, 30 } ) ) << "the exec records the value the setup prepared";
    EXPECT_EQ( preparedIds.size(), 4u ) << "walking the prepared list prepares nothing again";
    prepared.Reset();
    EXPECT_FALSE( prepared.Ready() ) << "the next frame's list is not the last frame's";
    EXPECT_TRUE( prepared.Draws().empty() );
}

// RDG-FAULT1 C3b gap 4: one broken material among several, through the path Render2D takes - PreparedDraws in the
// setup, UIMaterialFallback::Choose per draw (UIMaterialCache::DrawableOrDefault is Choose over the real entries,
// census RenderGraphCompile.UIMaterialDrawsFallBackPerDrawNotPerNode). Every draw declares a block, the broken one
// binds the default UI material (UIMatError), the healthy ones their own, and the log names the broken MATERIAL in
// one line across frames.
TEST( UIMaterialFallback, OneBrokenMaterialAmongSeveralThroughThePreparedPath )
{
    using R2D::UIMaterialFallback;
    using Fields = std::vector<std::string>;
    struct Entry
    {
        bool        Error = false;
        std::string AssetName;
        std::string Shader;
        Fields      Row; // the fields of the schema the row was built from
    };
    const auto layoutOf = []( const std::string& shader ) -> Fields
    {
        if ( shader == "UIGradient" )
        {
            return { "TopColor", "BottomColor" };
        }
        if ( shader == "UITint" )
        {
            return { "Tint" };
        }
        return {}; // UIMatError reads no row
    };
    const Entry                     gradient{ false, "UI_Gradient", "UIGradient", { "TopColor", "BottomColor" } };
    const Entry                     broken{ false, "UI_Broken", "UIGradient", { "TopColor" } }; // short row
    const Entry                     tint{ false, "UI_Tint", "UITint", { "Tint" } };
    const Entry                     error{ true, "", "UIMatError", {} };
    const std::vector<const Entry*> commands = { &gradient, &broken, &tint, &broken };

    UIMaterialFallback       fallback;
    std::vector<std::string> logs;
    int                      prepares = 0;
    const auto               prepare  = [&]( const Entry& entry )
    {
        ++prepares;
        const Fields layout = layoutOf( entry.Shader );
        return UIMaterialFallback::RowFault( !layout.empty(), layout, entry.Row, entry.Row.size() );
    };
    for ( int frame = 0; frame < 2; ++frame )
    {
        R2D::PreparedDraws<const Entry*> prepared;
        prepared.Prepare( commands,
                          [&]( const Entry* entry ) -> std::optional<const Entry*>
                          {
                              const Entry* chosen = fallback.Choose(
                                   *entry, [&]() { return &error; }, prepare,
                                   []( const Entry& e ) { return e.Shader; },
                                   [&]( const std::string& line ) { logs.push_back( line ); }, "UIMatError" );
                              if ( chosen == nullptr )
                              {
                                  return std::nullopt;
                              }
                              return chosen;
                          } );
        ASSERT_EQ( prepared.Draws().size(), commands.size() )
             << "every draw declares a block (frame " << frame << ")";
        EXPECT_EQ( prepared.Draws()[0].Value, &gradient );
        EXPECT_EQ( prepared.Draws()[1].Value, &error ) << "the broken material binds UIMatError";
        EXPECT_EQ( prepared.Draws()[2].Value, &tint ) << "a healthy material after the broken one draws itself";
        EXPECT_EQ( prepared.Draws()[3].Value, &error );
    }
    EXPECT_EQ( prepares, 12 ) << "per frame: one preparation per draw, plus the default's for each broken draw";
    ASSERT_EQ( logs.size(), 1u ) << "the broken material is reported once across frames";
    EXPECT_NE( logs[0].find( "material 'UI_Broken' (shader 'UIGradient')" ), std::string::npos ) << logs[0];
    EXPECT_EQ( logs[0].find( "UI_Broken" ), logs[0].rfind( "UI_Broken" ) ) << "named once: " << logs[0];
    EXPECT_NE( logs[0].find( "'UIMatError'" ), std::string::npos ) << logs[0];
    EXPECT_EQ( logs[0].find( "UI_Gradient" ), std::string::npos ) << "only the broken material is reported";
}

// RDG-FAULT1 C3b: a UI material built from a shader that has since HOT-RELOADED is rebuilt on the reload, not left
// on the default. The entry records its shader's reload generation (Shader::GetCodeGeneration, the key
// ShaderBindingLayoutCache uses); UIMaterialCache::Resolve runs UIMaterialFallback::RebuildIfReloaded on every hit
// before the frame's draws are prepared (census RenderGraphCompile.UIMaterialDrawsFallBackPerDrawNotPerNode).
// Here: the reload adds a parameter, so the old row no longer fits; bumping the generation rebuilds the row from
// the new layout and the next preparation draws the material itself, with nothing reported. Same generation: no
// rebuild.
TEST( UIMaterialFallback, AShaderReloadRebuildsTheMaterialInsteadOfFallingBack )
{
    using R2D::UIMaterialFallback;
    using Fields = std::vector<std::string>;
    struct Entry
    {
        bool        Error = false;
        std::string AssetName;
        Fields      Row;
        uint64_t    ShaderGeneration = 0;
    };
    Fields      layout     = { "TopColor", "BottomColor" }; // the shader's parameter layout, as compiled
    uint64_t    generation = 0;                             // Shader::GetCodeGeneration
    Entry       material{ false, "UI_Gradient", layout, generation };
    const Entry error{ true, "", {}, 0 };
    int         rebuilds = 0;
    const auto  rebuild  = [&]( Entry& stale )
    {
        ++rebuilds;
        stale.Row = layout; // a fresh build takes the reloaded shader's schema
        return true;
    };
    UIMaterialFallback       fallback;
    std::vector<std::string> logs;
    const auto               draw = [&]()
    {
        return fallback.Choose(
             material, [&]() { return &error; },
             [&]( const Entry& e ) {
                 return e.Error ? std::string()
                                : UIMaterialFallback::RowFault( true, layout, e.Row, e.Row.size() );
             },
             []( const Entry& ) { return std::string( "UIGradient" ); },
             [&]( const std::string& line ) { logs.push_back( line ); }, "UIMatError" );
    };

    EXPECT_FALSE( UIMaterialFallback::RebuildIfReloaded( material, generation, rebuild ) );
    EXPECT_EQ( draw(), &material );

    // The shader reloads with one more parameter.
    layout.emplace_back( "Glow" );
    ++generation;
    EXPECT_TRUE( UIMaterialFallback::RebuildIfReloaded( material, generation, rebuild ) )
         << "a bumped reload generation rebuilds the entry";
    EXPECT_EQ( material.ShaderGeneration, generation );
    EXPECT_EQ( draw(), &material ) << "the reloaded material draws itself, not the default";
    EXPECT_TRUE( logs.empty() ) << "nothing falls back, so nothing is reported";
    EXPECT_FALSE( UIMaterialFallback::RebuildIfReloaded( material, generation, rebuild ) )
         << "the same generation does not rebuild again";
    EXPECT_EQ( rebuilds, 1 );

    // A rebuild that fails is tried once per reload, not every frame.
    ++generation;
    layout.emplace_back( "Edge" );
    const auto failing = [&]( Entry& )
    {
        ++rebuilds;
        return false;
    };
    EXPECT_FALSE( UIMaterialFallback::RebuildIfReloaded( material, generation, failing ) );
    EXPECT_FALSE( UIMaterialFallback::RebuildIfReloaded( material, generation, failing ) );
    EXPECT_EQ( rebuilds, 2 );
    EXPECT_EQ( draw(), &error ) << "an entry that could not follow its shader falls back (reported once)";
    EXPECT_EQ( logs.size(), 1u );
}

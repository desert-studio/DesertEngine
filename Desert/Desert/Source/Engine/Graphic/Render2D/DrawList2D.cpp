#include "DrawList2D.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Desert::Graphic::Render2D
{
    namespace
    {
        Vertex2D LerpVertex( const Vertex2D& a, const Vertex2D& b, float t )
        {
            return Vertex2D{ a.Position + ( b.Position - a.Position ) * t, a.UV + ( b.UV - a.UV ) * t,
                             a.Color + ( b.Color - a.Color ) * t };
        }

        // Where the segment a..b crosses a plane, computed from an ORDER THAT DOES NOT DEPEND ON WHICH
        // TRIANGLE IS ASKING. Two triangles sharing an edge traverse it in opposite directions, and
        // `a + t*(b-a)` and `b + t'*(a-b)` are not the same float — a one-ULP gap along every internal
        // edge of a fan, which is a crack the rasterizer can and eventually does sample through. Sorting
        // the two endpoints by position makes the split point a function of the EDGE rather than of the
        // walk, so both sides land on the same bits.
        Vertex2D SplitEdge( const Vertex2D& a, const Vertex2D& b, float da, float db )
        {
            if ( da == db )
                return a; // parallel to the plane: nothing to interpolate, and no division to make
            const bool swap =
                 b.Position.x < a.Position.x || ( b.Position.x == a.Position.x && b.Position.y < a.Position.y );
            return swap ? LerpVertex( b, a, db / ( db - da ) ) : LerpVertex( a, b, da / ( da - db ) );
        }
    } // namespace

    void DrawList2D::Reset()
    {
        m_Vertices.clear();
        m_Indices.clear();
        m_Commands.clear();
        m_ClipStack.clear();
        m_Clip = ClipRegion2D{};
        m_TransformStack.clear();
        m_Transform    = glm::mat3( 1.0f );
        m_HasTransform = false;
    }

    void DrawList2D::PushTransform( const glm::mat3& xform )
    {
        // Composed, not replaced: a child's transform acts INSIDE its parent's, which is what makes a
        // rotated panel carry its sub-tree. The parent's matrix is the left operand because that is the
        // order the points travel — the child's first, then the parent's.
        m_TransformStack.push_back( m_Transform );
        m_Transform    = m_Transform * xform;
        m_HasTransform = true;
    }

    void DrawList2D::PopTransform()
    {
        if ( m_TransformStack.empty() )
        {
            m_Transform    = glm::mat3( 1.0f );
            m_HasTransform = false;
            return;
        }
        m_Transform = m_TransformStack.back();
        m_TransformStack.pop_back();
        // Back to the ground state EXACTLY: the flag goes false with the stack, so the outermost pop
        // restores the untouched-position path rather than leaving an identity multiply behind.
        m_HasTransform = !m_TransformStack.empty();
    }

    void DrawList2D::PushClipRect( const glm::vec2& min, const glm::vec2& max )
    {
        m_ClipStack.push_back( m_Clip );
        // The transform is the identity when nothing is pushed, and 1*x + 0*y + 0 is x exactly, so the
        // untransformed clip is still the rect it was written as — down to the bit.
        if ( !IntersectClipRegion( m_Clip, m_Transform, min, max ) )
        {
            LOG_WARN( "[Render2D] clip nesting deeper than {} oblique half-planes ({} rotated clippers) at "
                      "depth {}: the extra levels tighten the scissor box but not the quadrilateral, so this "
                      "clip is a superset of the exact one. The pointer is refused on the SAME superset.",
                      kMaxClipPlanes, kMaxClipPlanes / 4, m_ClipStack.size() );
        }
    }

    void DrawList2D::PopClipRect()
    {
        if ( m_ClipStack.empty() )
        {
            m_Clip = ClipRegion2D{};
            return;
        }
        m_Clip = m_ClipStack.back();
        m_ClipStack.pop_back();
    }

    glm::vec4 DrawList2D::ScissorBox() const
    {
        // W<=0 is the backend's "no scissor". An EMPTY region never reaches here — the primitives refuse to
        // emit at all — which is what stops a zero-area intersection from reading as "unclipped" and
        // splashing the whole viewport, the way it did while the clip was a bare vec4.
        return m_Clip.Bounded ? m_Clip.Box : glm::vec4( 0.0f, 0.0f, 0.0f, 0.0f );
    }

    DrawCommand& DrawList2D::CurrentCommand( const void* texture, bool text, const void* material )
    {
        const glm::vec4 scissor = ScissorBox();

        // A glass command is NEVER extended: its rect / radius / blur live in push constants, so anything
        // appended to it would be drawn by the glass pipeline with THAT rect's parameters. (Glass looks
        // like a solid batch — texture null, not text — so without this it would silently absorb the next
        // flat rectangle.)
        //
        // The OBLIQUE half of the clip is deliberately absent from this key: it never reaches the GPU, so
        // two batches cut by two different rotated clippers that happen to share a scissor box may still be
        // one draw call. That is the whole reason exact clipping costs no draw call at all.
        if ( !m_Commands.empty() && !m_Commands.back().Glass && m_Commands.back().Texture == texture &&
             m_Commands.back().Text == text && m_Commands.back().Material == material &&
             m_Commands.back().ClipRect == scissor )
            return m_Commands.back();

        DrawCommand cmd;
        cmd.Texture     = texture;
        cmd.Text        = text;
        cmd.Material    = material;
        cmd.ClipRect    = scissor;
        cmd.IndexOffset = static_cast<uint32_t>( m_Indices.size() );
        cmd.IndexCount  = 0;
        m_Commands.push_back( cmd );
        return m_Commands.back();
    }

    void DrawList2D::EmitClippedTriangle( DrawCommand& cmd, const Vertex2D& a, const Vertex2D& b,
                                          const Vertex2D& c )
    {
        // Sutherland-Hodgman. A convex polygon gains at most one corner per half-plane, so 3 + kMaxClipPlanes
        // bounds the working set and neither buffer ever allocates.
        std::array<Vertex2D, 3 + kMaxClipPlanes> buffers[2];
        buffers[0][0]  = a;
        buffers[0][1]  = b;
        buffers[0][2]  = c;
        int      front = 0;
        uint32_t count = 3;

        for ( uint32_t p = 0; p < m_Clip.PlaneCount && count >= 3; ++p )
        {
            const glm::vec3& plane = m_Clip.Planes[p];
            const auto&      poly  = buffers[front];
            auto&            next  = buffers[front ^ 1];
            uint32_t         out   = 0;
            for ( uint32_t i = 0; i < count; ++i )
            {
                const Vertex2D& cur = poly[i];
                const Vertex2D& nxt = poly[( i + 1 ) % count];
                const float     dc  = ClipPlaneDistance( plane, cur.Position );
                const float     dn  = ClipPlaneDistance( plane, nxt.Position );
                if ( dc >= 0.0f )
                    next[out++] = cur;
                if ( ( dc >= 0.0f ) != ( dn >= 0.0f ) )
                    next[out++] = SplitEdge( cur, nxt, dc, dn );
            }
            count = out;
            front ^= 1; // ping-pong: the polygon is up to 19 vertices, and copying it back would be 600
                        // bytes per plane per triangle for nothing
        }

        if ( count < 3 )
            return;

        const uint32_t base = static_cast<uint32_t>( m_Vertices.size() );
        m_Vertices.insert( m_Vertices.end(), buffers[front].begin(), buffers[front].begin() + count );
        for ( uint32_t i = 1; i + 1 < count; ++i )
        {
            m_Indices.push_back( base );
            m_Indices.push_back( base + i );
            m_Indices.push_back( base + i + 1 );
        }
        cmd.IndexCount += ( count - 2 ) * 3;
    }

    void DrawList2D::EmitPoly( DrawCommand& cmd, const Vertex2D* corners, uint32_t count )
    {
        if ( count < 3 )
            return;

        if ( ClippingOblique() )
        {
            for ( uint32_t i = 1; i + 1 < count; ++i )
                EmitClippedTriangle( cmd, corners[0], corners[i], corners[i + 1] );
            return;
        }

        const uint32_t base = static_cast<uint32_t>( m_Vertices.size() );
        m_Vertices.insert( m_Vertices.end(), corners, corners + count );
        for ( uint32_t i = 1; i + 1 < count; ++i )
        {
            m_Indices.push_back( base );
            m_Indices.push_back( base + i );
            m_Indices.push_back( base + i + 1 );
        }
        cmd.IndexCount += ( count - 2 ) * 3;
    }

    void DrawList2D::EmitClosedFan( DrawCommand& cmd, const Vertex2D& centre, const Vertex2D* rim,
                                    uint32_t rimCount )
    {
        if ( rimCount < 3 )
            return;

        if ( ClippingOblique() )
        {
            for ( uint32_t i = 0; i < rimCount; ++i )
                EmitClippedTriangle( cmd, centre, rim[i], rim[( i + 1 ) % rimCount] );
            return;
        }

        const uint32_t base = static_cast<uint32_t>( m_Vertices.size() );
        m_Vertices.push_back( centre );
        m_Vertices.insert( m_Vertices.end(), rim, rim + rimCount );
        for ( uint32_t i = 0; i < rimCount; ++i ) // closing the loop back to the first rim vertex
        {
            m_Indices.push_back( base );
            m_Indices.push_back( base + 1 + i );
            m_Indices.push_back( base + 1 + ( ( i + 1 ) % rimCount ) );
        }
        cmd.IndexCount += rimCount * 3;
    }

    void DrawList2D::EmitStrip( DrawCommand& cmd, const Vertex2D* pairs, uint32_t pairCount )
    {
        if ( pairCount < 2 )
            return;

        if ( ClippingOblique() )
        {
            for ( uint32_t i = 0; i + 1 < pairCount; ++i )
            {
                const size_t    at  = static_cast<size_t>( i ) * 2;
                const Vertex2D& o0  = pairs[at];
                const Vertex2D& in0 = pairs[at + 1];
                const Vertex2D& o1  = pairs[at + 2];
                const Vertex2D& in1 = pairs[at + 3];
                EmitClippedTriangle( cmd, o0, o1, in1 );
                EmitClippedTriangle( cmd, o0, in1, in0 );
            }
            return;
        }

        const uint32_t base = static_cast<uint32_t>( m_Vertices.size() );
        m_Vertices.insert( m_Vertices.end(), pairs, pairs + static_cast<size_t>( pairCount ) * 2 );
        for ( uint32_t i = 0; i + 1 < pairCount; ++i ) // two triangles bridge rim pair i -> i+1
        {
            const uint32_t o0 = base + i * 2, in0 = base + i * 2 + 1;
            const uint32_t o1 = base + ( i + 1 ) * 2, in1 = base + ( i + 1 ) * 2 + 1;
            m_Indices.push_back( o0 );
            m_Indices.push_back( o1 );
            m_Indices.push_back( in1 );
            m_Indices.push_back( o0 );
            m_Indices.push_back( in1 );
            m_Indices.push_back( in0 );
            cmd.IndexCount += 6;
        }
    }

    void DrawList2D::AddGlassRect( const glm::vec2& min, const glm::vec2& max, const glm::vec4& tint,
                                   float rounding, float blur01 )
    {
        if ( max.x <= min.x || max.y <= min.y || ClipRegionEmpty( m_Clip ) )
            return;

        // Its own command, always: the rect, its radius and its blur travel in push constants, so nothing
        // may be batched behind it. Opened by hand instead of through CurrentCommand for that reason.
        DrawCommand cmd;
        cmd.Texture     = nullptr;
        cmd.ClipRect    = ScissorBox();
        cmd.IndexOffset = static_cast<uint32_t>( m_Indices.size() );
        cmd.IndexCount  = 0;
        cmd.Glass       = true;
        cmd.GlassRect   = { min.x, min.y, max.x, max.y };
        cmd.GlassRound  = std::max( 0.0f, rounding );
        cmd.GlassLod    = std::clamp( blur01, 0.0f, 1.0f );
        // The mask is an SDF the FRAGMENT shader evaluates, so unlike every other primitive here it
        // cannot be transformed by moving vertices. It is given the rect in its OWN space plus the way
        // back from the screen instead, which is the same transform read the other way round.
        if ( m_HasTransform )
        {
            cmd.GlassInverse     = InverseTransform2D( m_Transform );
            const float perPixel = MeanScale2D( m_Transform );
            cmd.GlassFeather     = perPixel > 0.0f ? 1.0f / perPixel : 1.0f;
        }
        m_Commands.push_back( cmd );

        const Vertex2D corners[4] = { { Xf( { min.x, min.y } ), { 0.0f, 0.0f }, tint },
                                      { Xf( { max.x, min.y } ), { 1.0f, 0.0f }, tint },
                                      { Xf( { max.x, max.y } ), { 1.0f, 1.0f }, tint },
                                      { Xf( { min.x, max.y } ), { 0.0f, 1.0f }, tint } };
        EmitPoly( m_Commands.back(), corners, 4 );

        // GLASS IS CUT LIKE EVERYTHING ELSE, and the fact that it can be is why the clip lives on the CPU.
        // Its mask is an SDF over gl_FragCoord, so it does not care which triangles cover the fragment —
        // only that no fragment outside the clipper is covered at all. A stencil would have had to be told
        // about this pipeline separately; a push-constant mask could not have been, its 128 bytes are full.
        // Nothing survived => the command would draw zero indices, so it is taken back off rather than left
        // for the backend to skip.
        if ( m_Commands.back().IndexCount == 0 )
            m_Commands.pop_back();
    }

    void DrawList2D::AddQuad( const void* texture, const glm::vec2& min, const glm::vec2& max,
                              const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec4& color, bool text,
                              const void* material )
    {
        if ( ClipRegionEmpty( m_Clip ) )
            return;

        // Open/extend the batch BEFORE appending indices so a freshly opened command anchors its
        // IndexOffset at this quad's first index (not past it).
        DrawCommand& cmd = CurrentCommand( texture, text, material );

        // Corners: top-left, top-right, bottom-right, bottom-left (CW in a top-left-origin, y-down space).
        const Vertex2D corners[4] = { { Xf( { min.x, min.y } ), { uv0.x, uv0.y }, color },
                                      { Xf( { max.x, min.y } ), { uv1.x, uv0.y }, color },
                                      { Xf( { max.x, max.y } ), { uv1.x, uv1.y }, color },
                                      { Xf( { min.x, max.y } ), { uv0.x, uv1.y }, color } };
        EmitPoly( cmd, corners, 4 );
    }

    void DrawList2D::AddRectFilled( const glm::vec2& min, const glm::vec2& max, const glm::vec4& color,
                                    float rounding )
    {
        const float w = max.x - min.x, h = max.y - min.y;
        const float r = std::min( rounding, std::min( w, h ) * 0.5f );
        if ( r <= 0.5f )
        {
            // Sharp: null texture -> the backend binds its 1x1 white texel, so UVs are irrelevant (0..1).
            AddQuad( nullptr, min, max, { 0.0f, 0.0f }, { 1.0f, 1.0f }, color, false );
            return;
        }

        if ( ClipRegionEmpty( m_Clip ) )
            return;

        // Rounded: a triangle fan from the centre around a perimeter of four quarter-circle corner arcs (the
        // straight edges fall out between consecutive corner endpoints). y-down: angle 0=+x, PI/2=+y(down).
        constexpr float PI      = 3.14159265358979323846f;
        constexpr int   kSeg    = 6; // segments per corner
        constexpr int   kPerim  = 4 * ( kSeg + 1 );
        DrawCommand&    cmd     = CurrentCommand( nullptr, false );
        const glm::vec2 centre  = ( min + max ) * 0.5f;
        const Vertex2D  centreV = { Xf( centre ), { 0.5f, 0.5f }, color };

        const glm::vec2 cc[4] = { { min.x + r, min.y + r },
                                  { max.x - r, min.y + r },
                                  { max.x - r, max.y - r },
                                  { min.x + r, max.y - r } };
        const float     a0[4] = { PI, PI * 1.5f, 0.0f, PI * 0.5f }; // each arc sweeps +PI/2, clockwise (y-down)

        std::array<Vertex2D, kPerim> rim{};
        uint32_t                     perim = 0;
        for ( int c = 0; c < 4; ++c )
            for ( int s = 0; s <= kSeg; ++s )
            {
                const float     a = a0[c] + ( PI * 0.5f ) * ( static_cast<float>( s ) / kSeg );
                const glm::vec2 p = cc[c] + glm::vec2( std::cos( a ), std::sin( a ) ) * r;
                rim[perim++]      = { Xf( p ), { 0.5f, 0.5f }, color };
            }

        EmitClosedFan( cmd, centreV, rim.data(), perim );
    }

    void DrawList2D::AddImage( const void* texture, const glm::vec2& min, const glm::vec2& max,
                               const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec4& tint )
    {
        AddQuad( texture, min, max, uv0, uv1, tint, false );
    }

    void DrawList2D::AddMaterialRect( const void* material, const glm::vec2& min, const glm::vec2& max,
                                      const glm::vec4& tint )
    {
        // A null material would silently become an ordinary white-textured rect — the element would look
        // ALMOST right (its authored colour, no material) and nothing would say the fill never ran. The
        // resolve-or-error decision belongs to the caller, which is the only place that knows which handle
        // failed; Graphic::UIMaterialCache::Resolve never returns null for that reason.
        if ( !material || max.x <= min.x || max.y <= min.y )
            return;

        AddQuad( nullptr, min, max, { 0.0f, 0.0f }, { 1.0f, 1.0f }, tint, false, material );
    }

    void DrawList2D::AddRectFilledMultiColor( const glm::vec2& min, const glm::vec2& max,
                                              const glm::vec4& topColor, const glm::vec4& bottomColor )
    {
        if ( ClipRegionEmpty( m_Clip ) )
            return;

        DrawCommand& cmd = CurrentCommand( nullptr, false );

        // TL / TR carry the top colour, BR / BL the bottom colour -> a vertical gradient.
        const Vertex2D corners[4] = { { Xf( { min.x, min.y } ), { 0.0f, 0.0f }, topColor },
                                      { Xf( { max.x, min.y } ), { 1.0f, 0.0f }, topColor },
                                      { Xf( { max.x, max.y } ), { 1.0f, 1.0f }, bottomColor },
                                      { Xf( { min.x, max.y } ), { 0.0f, 1.0f }, bottomColor } };
        EmitPoly( cmd, corners, 4 );
    }

    void DrawList2D::AddRect( const glm::vec2& min, const glm::vec2& max, const glm::vec4& color, float thickness )
    {
        const float t = thickness;
        AddRectFilled( { min.x, min.y }, { max.x, min.y + t }, color );         // top
        AddRectFilled( { min.x, max.y - t }, { max.x, max.y }, color );         // bottom
        AddRectFilled( { min.x, min.y + t }, { min.x + t, max.y - t }, color ); // left
        AddRectFilled( { max.x - t, min.y + t }, { max.x, max.y - t }, color ); // right
    }

    void DrawList2D::AddText( const void* atlas, const glm::vec2& min, const glm::vec2& max, const glm::vec2& uv0,
                              const glm::vec2& uv1, const glm::vec4& color )
    {
        AddQuad( atlas, min, max, uv0, uv1, color, true );
    }

    void DrawList2D::AddTriangleFilled( const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2,
                                        const glm::vec4& color )
    {
        if ( ClipRegionEmpty( m_Clip ) )
            return;

        DrawCommand&   cmd        = CurrentCommand( nullptr, false );
        const Vertex2D corners[3] = { { Xf( p0 ), { 0.5f, 0.5f }, color },
                                      { Xf( p1 ), { 0.5f, 0.5f }, color },
                                      { Xf( p2 ), { 0.5f, 0.5f }, color } };
        EmitPoly( cmd, corners, 3 );
    }

    void DrawList2D::AddLine( const glm::vec2& a, const glm::vec2& b, const glm::vec4& color, float thickness )
    {
        const glm::vec2 d   = b - a;
        const float     len = std::sqrt( d.x * d.x + d.y * d.y );
        if ( len < 1e-4f || ClipRegionEmpty( m_Clip ) )
            return;
        const glm::vec2 n = glm::vec2( -d.y, d.x ) / len * ( thickness * 0.5f ); // perpendicular half-width

        DrawCommand&   cmd        = CurrentCommand( nullptr, false );
        const Vertex2D corners[4] = { { Xf( a - n ), { 0.5f, 0.5f }, color },
                                      { Xf( a + n ), { 0.5f, 0.5f }, color },
                                      { Xf( b + n ), { 0.5f, 0.5f }, color },
                                      { Xf( b - n ), { 0.5f, 0.5f }, color } };
        EmitPoly( cmd, corners, 4 );
    }

    void DrawList2D::AddRing( const glm::vec2& center, float outerRadius, float innerRadius,
                              const glm::vec4& colorA, const glm::vec4& colorB, int segments )
    {
        if ( outerRadius <= 0.0f || segments < 3 || ClipRegionEmpty( m_Clip ) )
            return;
        innerRadius = std::clamp( innerRadius, 0.0f, outerRadius );

        constexpr float TWO_PI = 6.28318530717958648f;
        DrawCommand&    cmd    = CurrentCommand( nullptr, false );

        // Two rims (outer, inner) per angular step; colour lerps A->B->A so the seam at 0/2PI is invisible.
        // Built in the list's own scratch buffer rather than a local vector: a ring is drawn per element per
        // frame, and this class's whole allocation policy is that Reset() keeps capacity.
        m_Scratch.clear();
        m_Scratch.reserve( static_cast<std::size_t>( segments + 1 ) * 2 );
        for ( int i = 0; i <= segments; ++i )
        {
            const float     f   = static_cast<float>( i ) / static_cast<float>( segments );
            const float     a   = TWO_PI * f;
            const float     t   = f < 0.5f ? f * 2.0f : ( 1.0f - f ) * 2.0f;
            const glm::vec4 col = colorA * ( 1.0f - t ) + colorB * t;
            const glm::vec2 dir( std::cos( a ), std::sin( a ) );
            m_Scratch.push_back( { Xf( center + dir * outerRadius ), { 0.5f, 0.5f }, col } );
            m_Scratch.push_back( { Xf( center + dir * innerRadius ), { 0.5f, 0.5f }, col } );
        }

        EmitStrip( cmd, m_Scratch.data(), static_cast<uint32_t>( segments + 1 ) );
    }
} // namespace Desert::Graphic::Render2D

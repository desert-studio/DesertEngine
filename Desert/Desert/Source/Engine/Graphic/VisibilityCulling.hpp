#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Math/AABBLanes.hpp>
#include <Common/Core/Math/Simd.hpp>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Desert::Graphic
{
    // WHETHER A PLACED MESH IS WORTH SUBMITTING. One rule, in a header, so the DECISION is reachable by a
    // test: 85 % of this engine's translation units are compiled by no suite, and a rule left inline in
    // MeshRenderer.cpp would have been one of them (see scripts/CI/UnreachedSources.sh).
    //
    // THE DEFAULT IS TO DRAW. A mesh with no extent — no submeshes, or bounds that were never filled in —
    // answers "I do not know where this is", and that must never be answered with "so it is not there".
    // Every failure of this function has to land on the side that costs a draw call, because the other
    // side costs a hole in the picture that nothing reports and only some camera angles show.
    [[nodiscard]] inline bool IsVisibleInView( const Core::Frustum& frustum, const glm::mat4& transform,
                                               const Common::Math::AABB& localBounds )
    {
        if ( Geometry::IsEmpty( localBounds ) )
        {
            return true;
        }
        return frustum.Intersects( Geometry::TransformBounds( transform, localBounds ) );
    }
    // The same rule for a box already in world space (a shadow caster's gathered box,
    // MeshRenderer::GatherShadowCasters): empty = no known extent = drawn.
    [[nodiscard]] inline bool IsVisibleInView( const Core::Frustum&      frustum,
                                               const Common::Math::AABB& worldBounds )
    {
        return Geometry::IsEmpty( worldBounds ) || frustum.Intersects( worldBounds );
    }

    // The same rule for a RUN of world boxes, four at a time (UE: the primitive-bounds batch of
    // FSceneRenderer's frustum cull): bit j of @p bits[j / 64] is IsVisibleInView( frustum, box @p first + j )
    // for every j below @p count, each bit the one the box-at-a-time call answers - IsEmpty per lane by the same
    // `Min > Max` compares, OR the lane's Core::Frustum::Intersects4 verdict. @p bits holds at least
    // (count + 63) / 64 words; those words are written whole, every bit at or past @p count clear.
    // first + count must not pass @p boxes.Size() (the lanes' padding covers the last group's overhang).
    // AddVisibleInViewBits ORs the same bits into @p bits instead (a union of views: a scrolled cascade's strips).
    //
    // VisibleInView4 is one group of that run: bit i is IsVisibleInView( frustum, box @p at + i ) for the four
    // lanes at..at + 3, unmasked (a lane at or past boxes.Size() is padding: the caller never reads its bit).
    // @p at must be below boxes.Size().
    [[nodiscard]] inline uint32_t VisibleInView4( const Core::Frustum&           frustum,
                                                  const Common::Math::AABBLanes& boxes,
                                                  const std::size_t              at ) noexcept
    {
        namespace S = Common::Math::Simd;
        const S::Mask4 emptyX =
             S::CompareGT( S::Load4( boxes.MinX.data() + at ), S::Load4( boxes.MaxX.data() + at ) );
        const S::Mask4 emptyY =
             S::CompareGT( S::Load4( boxes.MinY.data() + at ), S::Load4( boxes.MaxY.data() + at ) );
        const S::Mask4 emptyZ =
             S::CompareGT( S::Load4( boxes.MinZ.data() + at ), S::Load4( boxes.MaxZ.data() + at ) );
        uint32_t lanes = S::MaskBits( S::MaskOr( S::MaskOr( emptyX, emptyY ), emptyZ ) );
        if ( lanes != 0xFu )
            lanes |= frustum.Intersects4( boxes, at );
        return lanes;
    }

    inline void AddVisibleInViewBits( const Core::Frustum& frustum, const Common::Math::AABBLanes& boxes,
                                      const std::size_t first, const std::size_t count,
                                      const std::span<uint64_t> bits ) noexcept
    {
        for ( std::size_t j = 0; j < count; j += 4u )
        {
            uint32_t lanes = VisibleInView4( frustum, boxes, first + j );
            if ( count - j < 4u )
                lanes &= ( 1u << ( count - j ) ) - 1u;
            bits[j / 64u] |= static_cast<uint64_t>( lanes ) << ( j % 64u );
        }
    }

    inline void VisibleInViewBits( const Core::Frustum& frustum, const Common::Math::AABBLanes& boxes,
                                   const std::size_t first, const std::size_t count,
                                   const std::span<uint64_t> bits ) noexcept
    {
        const std::size_t words = ( count + 63u ) / 64u;
        for ( std::size_t w = 0; w < words; ++w )
            bits[w] = 0u;
        AddVisibleInViewBits( frustum, boxes, first, count, bits );
    }

    [[nodiscard]] inline bool VisibleBit( const std::span<const uint64_t> bits, const std::size_t j ) noexcept
    {
        return ( ( bits[j / 64u] >> ( j % 64u ) ) & 1u ) != 0u;
    }

    // A WALK THAT ASKS ONLY SOME BOXES OF A RUN (UE: the primitive-bounds batch, asked after the per-primitive
    // relevance a view already decided - a record the GPU bins hold never reaches the frustum test): the four-box
    // group holding box @p index is tested once, by @p groupOf( base ) (base = index rounded down to four, a
    // VisibleInView4-shaped verdict, bit i for box base + i), the first time a box of it is asked; the next boxes
    // of the same group read its bits. Each answer is the bit the box-at-a-time test gives, so a walk asking its
    // boxes in increasing order keeps the same set it kept one box at a time. A walk restarting below the group
    // it last asked (another chunk) uses its own cursor.
    class VisibleInViewQuads final
    {
    public:
        template <typename GroupOf>
        [[nodiscard]] bool Visible( const std::size_t index, GroupOf&& groupOf ) noexcept
        {
            const std::size_t base = index & ~std::size_t{ 3u };
            if ( !m_Valid || base != m_Base )
            {
                m_Bits  = groupOf( base );
                m_Base  = base;
                m_Valid = true;
            }
            return ( ( m_Bits >> ( index - base ) ) & 1u ) != 0u;
        }

    private:
        std::size_t m_Base  = 0;
        uint32_t    m_Bits  = 0;
        bool        m_Valid = false;
    };

    // HOW FAR A VIEW'S LENS STRETCHES EVERY DRAW AND CULL DISTANCE. Distances are authored for a 90-degree
    // HORIZONTAL lens; through a lens of horizontal focal scale k an object at distance d is as wide on screen as
    // it would be at d / k through that one, so it stays drawn to k times the authored distance. A zoomed-in
    // camera sees farther, a wide one nearer.
    // HORIZONTAL, |Projection[0][0]| alone, and not LOD's max of both focal terms: that max is the larger screen
    // axis, so it grows with the viewport's aspect — a 90-degree editor view (FOV is horizontal there, UE's
    // Maintain X FOV) 1.67 times wider than tall read 1.67 and drew a 5000 cm limit out to 8370 cm. A draw
    // distance is a property of the lens, not of the window's shape; UE's DesiredFOV is horizontal too.
    // UE scales MaxDrawDistance and the foliage CullDistance by ViewDistanceScale x
    // CalculateFieldOfViewDistanceScale(View.DesiredFOV) (SceneVisibility.cpp; Engine/Public/UnrealEngine.h), a
    // per-project lerp over FOV in cvars; here the lens itself is the scale, with no curve to tune.
    // 1 for an orthographic projection (|Projection[2][3]| = 0): its "distance" is not a size.
    [[nodiscard]] inline float DrawDistanceScale( const Geometry::LODView& view )
    {
        if ( std::abs( view.Projection[2][3] ) == 0.0f )
            return 1.0f;
        const float focal = std::abs( view.Projection[0][0] );
        return focal > 0.0f ? focal : 1.0f;
    }

    // WHETHER A PLACED MESH IS WITHIN ITS MAX DRAW DISTANCE (UE's LDMaxDrawDistance) from @p view: the distance
    // to its bounds' centre, against the authored limit stretched by the lens. maxDrawDistance <= 0: no limit.
    // A mesh with no extent is measured at its origin — it still has a place, unlike in the frustum test.
    [[nodiscard]] inline bool IsWithinDrawDistance( const Geometry::LODView& view, const glm::mat4& transform,
                                                    const Common::Math::AABB& localBounds, float maxDrawDistance )
    {
        if ( maxDrawDistance <= 0.0f )
            return true;
        const glm::vec3 localCentre =
             Geometry::IsEmpty( localBounds ) ? glm::vec3( 0.0f ) : 0.5f * ( localBounds.Min + localBounds.Max );
        const glm::vec3 centre = glm::vec3( transform * glm::vec4( localCentre, 1.0f ) );
        const glm::vec3 offset = centre - view.Origin;
        const float     limit  = maxDrawDistance * DrawDistanceScale( view );
        return glm::dot( offset, offset ) <= limit * limit;
    }
} // namespace Desert::Graphic

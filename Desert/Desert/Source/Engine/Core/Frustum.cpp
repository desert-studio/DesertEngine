#include "Frustum.hpp"

#include <array>

namespace Desert::Core
{

    Frustum::Frustum( const glm::mat4& projection, const glm::mat4& view )
    {
        Rebuild( projection, view );
    }

    // The point and sphere tests, four planes at a time like Intersects (Frustum.hpp): per lane the plain loop's
    // arithmetic in its order, ((x*nx + y*ny) + z*nz) + d, and its compare.
    bool Frustum::IsInside( const glm::vec3& point ) const noexcept
    {
        namespace S          = Common::Math::Simd;
        const S::Float4 zero = S::Zero();
        const S::Float4 x    = S::Splat( point.x );
        const S::Float4 y    = S::Splat( point.y );
        const S::Float4 z    = S::Splat( point.z );
        for ( uint32_t group = 0; group < 2u; ++group )
        {
            const uint32_t  base = group * 4u;
            const S::Float4 dot  = S::Add( S::Add( S::Mul( x, S::Load4( m_Lanes.Nx + base ) ),
                                                 S::Mul( y, S::Load4( m_Lanes.Ny + base ) ) ),
                                          S::Mul( z, S::Load4( m_Lanes.Nz + base ) ) );
            // Outside on `distance < 0` (not `!( distance >= 0 )`): a NaN distance keeps the point, as it did.
            const S::Float4 distance = S::Add( dot, S::Load4( m_Lanes.D + base ) );
            const uint32_t  outside  = S::MaskBits( S::CompareLT( distance, zero ) );
            if ( ( outside & kGroupLanes[group] ) != 0u )
                return false;
        }
        return true;
    }

    bool Frustum::IntersectsSphere( const glm::vec3& center, const float radius ) const noexcept
    {
        namespace S             = Common::Math::Simd;
        const S::Float4 reach   = S::Splat( -radius );
        const S::Float4 x       = S::Splat( center.x );
        const S::Float4 y       = S::Splat( center.y );
        const S::Float4 z       = S::Splat( center.z );
        for ( uint32_t group = 0; group < 2u; ++group )
        {
            const uint32_t  base = group * 4u;
            const S::Float4 dot  = S::Add( S::Add( S::Mul( x, S::Load4( m_Lanes.Nx + base ) ),
                                                 S::Mul( y, S::Load4( m_Lanes.Ny + base ) ) ),
                                          S::Mul( z, S::Load4( m_Lanes.Nz + base ) ) );
            const S::Float4 distance = S::Add( dot, S::Load4( m_Lanes.D + base ) );
            const uint32_t  kept     = S::MaskBits( S::CompareGE( distance, reach ) );
            if ( ( kept & kGroupLanes[group] ) != kGroupLanes[group] )
                return false;
        }
        return true;
    }

    uint32_t Frustum::IntersectsSphere4( const Common::Math::SphereQuad& spheres ) const noexcept
    {
        namespace S       = Common::Math::Simd;
        const S::Float4 x = S::Load4( spheres.X );
        const S::Float4 y = S::Load4( spheres.Y );
        const S::Float4 z = S::Load4( spheres.Z );
        // -radius per lane, the same negation as IntersectsSphere's Splat( -radius ) (not 0 - r: same value).
        float negated[4];
        for ( int lane = 0; lane < 4; ++lane )
            negated[lane] = -spheres.R[lane];
        const S::Float4 reach = S::Load4( negated );
        uint32_t        kept  = 0xFu;
        // Planes 0..5 (6 and 7 are padding, kGroupLanes); a batch every lane of which is out stops the walk.
        for ( uint32_t plane = 0; plane < 6u && kept != 0u; ++plane )
        {
            // IntersectsSphere's Mul( x, nx ) order per lane (IEEE multiply commutes; the adds keep its order).
            const S::Float4 dot = S::Add(
                 S::Add( S::Mul( x, S::Splat( m_Lanes.Nx[plane] ) ), S::Mul( y, S::Splat( m_Lanes.Ny[plane] ) ) ),
                 S::Mul( z, S::Splat( m_Lanes.Nz[plane] ) ) );
            const S::Float4 distance = S::Add( dot, S::Splat( m_Lanes.D[plane] ) );
            kept &= S::MaskBits( S::CompareGE( distance, reach ) );
        }
        return kept;
    }

    void Frustum::Rebuild( const glm::mat4& projection, const glm::mat4& view )
    {
        glm::mat4 viewProjection = projection * view;

        // Built per plane, then stored lane per plane (m_Lanes, the only copy).
        std::array<Plane, 6> planes;

        // Left plane
        planes[PLANE_LEFT].Normal.x = viewProjection[0][3] + viewProjection[0][0];
        planes[PLANE_LEFT].Normal.y = viewProjection[1][3] + viewProjection[1][0];
        planes[PLANE_LEFT].Normal.z = viewProjection[2][3] + viewProjection[2][0];
        planes[PLANE_LEFT].Distance = viewProjection[3][3] + viewProjection[3][0];

        // Right plane
        planes[PLANE_RIGHT].Normal.x = viewProjection[0][3] - viewProjection[0][0];
        planes[PLANE_RIGHT].Normal.y = viewProjection[1][3] - viewProjection[1][0];
        planes[PLANE_RIGHT].Normal.z = viewProjection[2][3] - viewProjection[2][0];
        planes[PLANE_RIGHT].Distance = viewProjection[3][3] - viewProjection[3][0];

        // Bottom plane
        planes[PLANE_DOWN].Normal.x = viewProjection[0][3] + viewProjection[0][1];
        planes[PLANE_DOWN].Normal.y = viewProjection[1][3] + viewProjection[1][1];
        planes[PLANE_DOWN].Normal.z = viewProjection[2][3] + viewProjection[2][1];
        planes[PLANE_DOWN].Distance = viewProjection[3][3] + viewProjection[3][1];

        // Top plane
        planes[PLANE_UP].Normal.x = viewProjection[0][3] - viewProjection[0][1];
        planes[PLANE_UP].Normal.y = viewProjection[1][3] - viewProjection[1][1];
        planes[PLANE_UP].Normal.z = viewProjection[2][3] - viewProjection[2][1];
        planes[PLANE_UP].Distance = viewProjection[3][3] - viewProjection[3][1];

        // Near and far come from the DEPTH convention, and are the only two planes that do — the side
        // planes are the same in every clip space. Vulkan keeps 0 <= z <= w (not OpenGL's -w <= z <= w),
        // and the engine's projections are REVERSED-Z, so z = w on the near plane and z = 0 on the far
        // one (Core/Projection.hpp). The half-space tests are therefore `w - z >= 0` for near and
        // `z >= 0` for far — NOT the `w + z` / `w - z` pair a GL-convention derivation gives.

        // Near plane: row3 - row2
        planes[PLANE_NEAR].Normal.x = viewProjection[0][3] - viewProjection[0][2];
        planes[PLANE_NEAR].Normal.y = viewProjection[1][3] - viewProjection[1][2];
        planes[PLANE_NEAR].Normal.z = viewProjection[2][3] - viewProjection[2][2];
        planes[PLANE_NEAR].Distance = viewProjection[3][3] - viewProjection[3][2];

        // Far plane: row2 alone
        planes[PLANE_FAR].Normal.x = viewProjection[0][2];
        planes[PLANE_FAR].Normal.y = viewProjection[1][2];
        planes[PLANE_FAR].Normal.z = viewProjection[2][2];
        planes[PLANE_FAR].Distance = viewProjection[3][2];

        // Normalize all planes
        for ( uint32_t lane = 0; lane < 6u; ++lane )
        {
            Plane&      plane  = planes[lane];
            const float length = glm::length( plane.Normal );
            plane.Normal /= length;
            plane.Distance /= length;
            SetPlane( lane, plane );
        }
    }

} // namespace Desert::Core
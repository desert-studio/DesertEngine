#include "Frustum.hpp"

namespace Desert::Core
{

    Frustum::Frustum( const glm::mat4& projection, const glm::mat4& view )
    {
        Rebuild( projection, view );
    }

    bool Frustum::IsInside( const glm::vec3& point ) const
    {
        for ( const auto& plane : m_Planes )
        {
            if ( plane.GetDistance( point ) < 0.0f )
            {
                return false;
            }
        }
        return true;
    }

    bool Frustum::Intersects( const Common::Math::AABB& worldBox ) const
    {
        for ( const auto& plane : m_Planes )
        {
            // The corner farthest along this plane's normal. Written per axis rather than with a select
            // over the whole vector because that is exactly what the test is: three independent choices.
            const glm::vec3 positiveVertex( plane.Normal.x >= 0.0f ? worldBox.Max.x : worldBox.Min.x,
                                            plane.Normal.y >= 0.0f ? worldBox.Max.y : worldBox.Min.y,
                                            plane.Normal.z >= 0.0f ? worldBox.Max.z : worldBox.Min.z );
            if ( plane.GetDistance( positiveVertex ) < 0.0f )
            {
                return false;
            }
        }
        return true;
    }

    void Frustum::Rebuild( const glm::mat4& projection, const glm::mat4& view )
    {
        glm::mat4 viewProjection = projection * view;

        // Left plane
        m_Planes[PLANE_LEFT].Normal.x = viewProjection[0][3] + viewProjection[0][0];
        m_Planes[PLANE_LEFT].Normal.y = viewProjection[1][3] + viewProjection[1][0];
        m_Planes[PLANE_LEFT].Normal.z = viewProjection[2][3] + viewProjection[2][0];
        m_Planes[PLANE_LEFT].Distance = viewProjection[3][3] + viewProjection[3][0];

        // Right plane
        m_Planes[PLANE_RIGHT].Normal.x = viewProjection[0][3] - viewProjection[0][0];
        m_Planes[PLANE_RIGHT].Normal.y = viewProjection[1][3] - viewProjection[1][0];
        m_Planes[PLANE_RIGHT].Normal.z = viewProjection[2][3] - viewProjection[2][0];
        m_Planes[PLANE_RIGHT].Distance = viewProjection[3][3] - viewProjection[3][0];

        // Bottom plane
        m_Planes[PLANE_DOWN].Normal.x = viewProjection[0][3] + viewProjection[0][1];
        m_Planes[PLANE_DOWN].Normal.y = viewProjection[1][3] + viewProjection[1][1];
        m_Planes[PLANE_DOWN].Normal.z = viewProjection[2][3] + viewProjection[2][1];
        m_Planes[PLANE_DOWN].Distance = viewProjection[3][3] + viewProjection[3][1];

        // Top plane
        m_Planes[PLANE_UP].Normal.x = viewProjection[0][3] - viewProjection[0][1];
        m_Planes[PLANE_UP].Normal.y = viewProjection[1][3] - viewProjection[1][1];
        m_Planes[PLANE_UP].Normal.z = viewProjection[2][3] - viewProjection[2][1];
        m_Planes[PLANE_UP].Distance = viewProjection[3][3] - viewProjection[3][1];

        // Near and far come from the DEPTH convention, and are the only two planes that do — the side
        // planes are the same in every clip space. Vulkan keeps 0 <= z <= w (not OpenGL's -w <= z <= w),
        // and the engine's projections are REVERSED-Z, so z = w on the near plane and z = 0 on the far
        // one (Core/Projection.hpp). The half-space tests are therefore `w - z >= 0` for near and
        // `z >= 0` for far — NOT the `w + z` / `w - z` pair a GL-convention derivation gives.

        // Near plane: row3 - row2
        m_Planes[PLANE_NEAR].Normal.x = viewProjection[0][3] - viewProjection[0][2];
        m_Planes[PLANE_NEAR].Normal.y = viewProjection[1][3] - viewProjection[1][2];
        m_Planes[PLANE_NEAR].Normal.z = viewProjection[2][3] - viewProjection[2][2];
        m_Planes[PLANE_NEAR].Distance = viewProjection[3][3] - viewProjection[3][2];

        // Far plane: row2 alone
        m_Planes[PLANE_FAR].Normal.x = viewProjection[0][2];
        m_Planes[PLANE_FAR].Normal.y = viewProjection[1][2];
        m_Planes[PLANE_FAR].Normal.z = viewProjection[2][2];
        m_Planes[PLANE_FAR].Distance = viewProjection[3][2];

        // Normalize all planes
        for ( auto& plane : m_Planes )
        {
            float length = glm::length( plane.Normal );
            plane.Normal /= length;
            plane.Distance /= length;
        }
    }

} // namespace Desert::Core
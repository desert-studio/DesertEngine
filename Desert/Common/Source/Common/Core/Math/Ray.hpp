#pragma once

#include "AABB.hpp"

#include <cstdint>

namespace Common::Math
{
    /**
     * Ray class representing a 3D ray with origin and direction.
     * All operations assume right-handed coordinate system.
     */
    class Ray
    {
    public:
        glm::vec3 Origin;    // Ray origin point (in world space coordinates)
        glm::vec3 Direction; // Ray direction vector (normalized, in world space)

        /**
         * Constructs a ray with automatic direction normalization
         * @param origin - Starting point in world space
         * @param direction - Direction vector (will be normalized)
         */
        Ray( const glm::vec3& origin, const glm::vec3& direction )
             : Origin( origin ), Direction( glm::normalize( direction ) )
        {
        }

        // === Core functionality ===

        /**
         * Gets point along the ray at distance t
         * @param t - Distance from Origin along Direction
         * @return Point in world space coordinates
         */
        glm::vec3 GetPoint( float t ) const
        {
            return Origin + Direction * t;
        }

        /**
         * Tests intersection between the ray and an Axis-Aligned Bounding Box (AABB)
         * @param aabb - The bounding box to test against
         * @param t - Output parameter for intersection distance along the ray
         * @return True if intersection occurs, false otherwise
         */
        bool IntersectsAABB( const AABB& aabb, float& t ) const
        {
            glm::vec3 dirfrac;
            // r.dir is unit direction vector of ray
            dirfrac.x = 1.0f / Direction.x;
            dirfrac.y = 1.0f / Direction.y;
            dirfrac.z = 1.0f / Direction.z;
            // lb is the corner of AABB with minimal coordinates - left bottom, rt is maximal corner
            // r.org is origin of ray
            const glm::vec3& lb = aabb.Min;
            const glm::vec3& rt = aabb.Max;
            float            t1 = ( lb.x - Origin.x ) * dirfrac.x;
            float            t2 = ( rt.x - Origin.x ) * dirfrac.x;
            float            t3 = ( lb.y - Origin.y ) * dirfrac.y;
            float            t4 = ( rt.y - Origin.y ) * dirfrac.y;
            float            t5 = ( lb.z - Origin.z ) * dirfrac.z;
            float            t6 = ( rt.z - Origin.z ) * dirfrac.z;

            float tmin = glm::max( glm::max( glm::min( t1, t2 ), glm::min( t3, t4 ) ), glm::min( t5, t6 ) );
            float tmax = glm::min( glm::min( glm::max( t1, t2 ), glm::max( t3, t4 ) ), glm::max( t5, t6 ) );

            // if tmax < 0, ray (line) is intersecting AABB, but the whole AABB is behind us
            if ( tmax < 0 )
            {
                t = tmax;
                return false;
            }

            // if tmin > tmax, ray doesn't intersect AABB
            if ( tmin > tmax )
            {
                t = tmax;
                return false;
            }

            t = tmin;
            return true;
        }

        // === Coordinate space transformations ===

        /**
         * Transforms ray from world space to object's local space
         * (Used for intersection tests with AABB which is typically defined in local space)
         * @param transform - Object's model matrix (transforms from local to world space)
         * @return Ray in object's local coordinate space
         */
        Ray ToLocalSpace( const glm::mat4& transform ) const
        {
            glm::mat4 invTransform = glm::inverse( transform );
            // Transform origin as point (w=1), including translation
            glm::vec3 localOrigin = invTransform * glm::vec4( Origin, 1.0f );
            // Transform direction as vector (w=0), ignoring translation
            glm::vec3 localDirection = glm::mat3( invTransform ) * Direction;
            return Ray( localOrigin, localDirection );
        }

        // The distance along THIS (world) ray of a hit found on `local`, this ray taken into an object's
        // space by ToLocalSpace( transform ), at `localT`. ToLocalSpace renormalises the direction, so a
        // local t is measured in the object's own units: under a scale it is not a world distance, and two
        // objects' local t cannot be compared to find the nearer one.
        [[nodiscard]] float WorldDistanceOf( const Ray& local, float localT, const glm::mat4& transform ) const
        {
            const glm::vec3 point = transform * glm::vec4( local.GetPoint( localT ), 1.0f );
            return glm::dot( point - Origin, Direction );
        }

        /**
         * Transforms ray from local space to world space
         * @param transform - Object's model matrix
         * @return Ray in world space coordinates
         */
        Ray ToWorldSpace( const glm::mat4& transform ) const
        {
            // Transform origin as point (w=1)
            glm::vec3 worldOrigin = transform * glm::vec4( Origin, 1.0f );
            // Transform direction as vector (w=0)
            glm::vec3 worldDirection = transform * glm::vec4( Direction, 0.0f );
            return Ray( worldOrigin, worldDirection );
        }

        // === Static constructors ===

        /**
         * Creates ray from screen position (mouse coordinates)
         * Note: Resulting ray is in world space coordinates
         * @param mousePosition - Screen coordinates in pixels
         * @param projection - Camera projection matrix
         * @param view - Camera view matrix
         * @param cameraPos - Camera position in world space
         * @param width - Viewport width
         * @param height - Viewport height
         * @return Ray in world space originating at camera
         */
        static Ray FromScreenPosition( const glm::vec2& mousePosition, const glm::mat4& projection,
                                       const glm::mat4& view, const glm::vec3& cameraPos,
                                       const uint32_t viewportWidth, const uint32_t viewportHeight )
        {
            // Convert screen coordinates to NDC [-1, 1]
            const float x = ( 2.0f * mousePosition.x ) / viewportWidth - 1.0f;
            const float y = 1.0f - ( 2.0f * mousePosition.y ) / viewportHeight;

            // A clip-space point on the ray. THE z HERE IS ARBITRARY under a perspective projection and
            // is not a depth-convention assumption, which matters now that the engine renders reversed-Z:
            // the inverse of a perspective matrix has (0, 0, 0, -1) as its third row, so the x, y and z of
            // the result below depend only on the pixel — the clip z leaks into w alone, and w is unused
            // because a direction needs no perspective divide.
            const glm::vec4 clipCoords = { x, y, -1.0f, 1.0f };

            // Transform back to world space
            const auto inverseProj = glm::inverse( projection );
            const auto inverseView = glm::inverse( glm::mat3( view ) );

            const glm::vec4 ray    = inverseProj * clipCoords;
            const glm::vec3 rayDir = inverseView * glm::vec3( ray );

            // Normalize direction vector (origin at camera position)
            return Ray( cameraPos, glm::normalize( glm::vec3( rayDir ) ) );
        }
    };

} // namespace Common::Math
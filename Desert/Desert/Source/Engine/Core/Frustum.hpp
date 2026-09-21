#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <glm/glm.hpp>

#include <array> // m_Planes — reached transitively inside the engine, but not by a test that includes this alone

namespace Desert::Core
{
    class Camera;

    enum FrustumPlane
    {
        PLANE_NEAR = 0,
        PLANE_LEFT,
        PLANE_RIGHT,
        PLANE_UP,
        PLANE_DOWN,
        PLANE_FAR,
    };

    class Frustum final
    {
    public:
        struct Plane
        {
            glm::vec3 Normal;
            float     Distance;

            float GetDistance( const glm::vec3& point ) const
            {
                return glm::dot( point, Normal ) + Distance;
            }
        };

    public:
        Frustum() = default;

        explicit Frustum( const glm::mat4& projection, const glm::mat4& view );

        void Rebuild( const glm::mat4& projection, const glm::mat4& view );

        bool IsInside( const glm::vec3& point ) const;

        // THE TEST THE RENDER PATH USES, and the reason the point test above could never have been it.
        //
        // An object is not a point. A building whose CENTRE is behind the camera can still fill half the
        // screen, and a wall the camera stands next to has its centre outside every side plane. Culling on
        // the centre deletes both — silently, and only from certain angles, which is the worst shape a
        // rendering defect can have.
        //
        // Standard "positive vertex" test: for each plane take the box corner FARTHEST along that plane's
        // normal; if even that corner is on the negative side, every corner is, and the box is rejected.
        // Six dot products and no square roots.
        //
        // CONSERVATIVE BY CONSTRUCTION. A box wedged past the corner where two planes meet can satisfy all
        // six half-space tests while touching no part of the frustum, so this says "visible" slightly more
        // often than geometry requires. That error costs one draw call. The opposite error costs a hole in
        // the picture, and this function will not trade in that direction.
        [[nodiscard]] bool Intersects( const Common::Math::AABB& worldBox ) const;

    private:
        std::array<Plane, 6> m_Planes;
    };
} // namespace Desert::Core
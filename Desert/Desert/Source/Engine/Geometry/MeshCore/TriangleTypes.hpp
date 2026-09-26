// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/TriangleTypes.h:261-273,323-325, adapted: namespace
// Desert::Geometry; only the Triangle3 members DistPoint3Triangle3 and MeshSurfacePath use (vertices and
// construction). The 2D triangle and the containment/normal helpers are not ported.
#pragma once

#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <typename RealType>
    struct Triangle3
    {
        glm::vec<3, RealType> V[3]{};

        Triangle3() = default;

        Triangle3( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1,
                   const glm::vec<3, RealType>& V2 )
        {
            V[0] = V0;
            V[1] = V1;
            V[2] = V2;
        }
    };

    using Triangle3f = Triangle3<float>;
    using Triangle3d = Triangle3<double>;
} // namespace Desert::Geometry

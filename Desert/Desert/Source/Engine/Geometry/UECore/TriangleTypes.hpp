// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/TriangleTypes.h:261-273,323-325, adapted: namespace
// Desert::Geometry; only the TTriangle3 members TDistPoint3Triangle3 and FMeshSurfacePath use (vertices and
// construction). The 2D triangle and the containment/normal helpers are not ported.
#pragma once

#include "Engine/Geometry/UECore/VectorTypes.hpp"

namespace Desert::Geometry
{
    template <typename RealType>
    struct TTriangle3
    {
        glm::vec<3, RealType> V[3]{};

        TTriangle3() = default;

        TTriangle3( const glm::vec<3, RealType>& V0, const glm::vec<3, RealType>& V1,
                    const glm::vec<3, RealType>& V2 )
        {
            V[0] = V0;
            V[1] = V1;
            V[2] = V2;
        }
    };

    using FTriangle3f = TTriangle3<float>;
    using FTriangle3d = TTriangle3<double>;
} // namespace Desert::Geometry

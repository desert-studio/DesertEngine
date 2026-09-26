// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/HoleFiller.h,
// adapted: namespace Desert::Geometry, UE Core as std/glm.
#pragma once

#include <vector>


namespace Desert::Geometry
{
    class IHoleFiller
    {
    public:
        std::vector<int> m_NewTriangles;

        virtual ~IHoleFiller()                = default;
        virtual bool Fill( int GroupID )      = 0;
    };
} // namespace Desert::Geometry

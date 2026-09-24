// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/HoleFiller.h,
// adapted: namespace Desert::Geometry, UE Core via UECore.hpp.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

namespace Desert::Geometry
{
    class IHoleFiller
    {
    public:
        TArray<int> NewTriangles;

        virtual ~IHoleFiller()                = default;
        virtual bool Fill( int GroupID = -1 ) = 0;
    };
} // namespace Desert::Geometry

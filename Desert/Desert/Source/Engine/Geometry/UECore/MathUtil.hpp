// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MathUtil.h:18-78, adapted: only ZeroTolerance
// survives; the rest of UE's TMathUtil is spelled with <cmath>, <limits> and glm constants at the call sites.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include <glm/gtc/constants.hpp>

#include <limits>

namespace Desert::Geometry
{
    // UE's per-precision "treat as zero" threshold; only float and double have one, any other type fails to
    // compile.
    template <typename RealType>
    inline constexpr RealType ZeroTolerance = RealType::NoZeroToleranceForThisType;
    template <>
    inline constexpr float ZeroTolerance<float> = 1e-06f;
    template <>
    inline constexpr double ZeroTolerance<double> = 1e-08;
} // namespace Desert::Geometry

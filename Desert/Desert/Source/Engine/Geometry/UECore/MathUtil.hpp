// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MathUtil.h:18-78,149-215, adapted: only the
// float/double constants and the TMathUtil functions the FDynamicMesh3 port calls, bodies over <cmath>;
// SafeLargeValue for float uses FLT_MAX (UE_LARGE_WORLD_MAX is an engine-config value we do not have).
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include <cfloat>

namespace Desert::Geometry
{
    template <typename RealType>
    struct TMathUtilConstants;

    template <>
    struct TMathUtilConstants<float>
    {
        static constexpr float Epsilon        = FLT_EPSILON;
        static constexpr float ZeroTolerance  = 1e-06f;
        static constexpr float MaxReal        = FLT_MAX;
        static constexpr float SafeLargeValue = FLT_MAX;
        static constexpr float Pi             = 3.1415926535897932384626433832795f;
        static constexpr float FourPi         = 4.0f * Pi;
        static constexpr float TwoPi          = 2.0f * Pi;
        static constexpr float HalfPi         = 0.5f * Pi;
    };

    template <>
    struct TMathUtilConstants<double>
    {
        static constexpr double Epsilon        = DBL_EPSILON;
        static constexpr double ZeroTolerance  = 1e-08;
        static constexpr double MaxReal        = DBL_MAX;
        static constexpr double SafeLargeValue = (double)FLT_MAX;
        static constexpr double Pi             = 3.1415926535897932384626433832795;
        static constexpr double FourPi         = 4.0 * Pi;
        static constexpr double TwoPi          = 2.0 * Pi;
        static constexpr double HalfPi         = 0.5 * Pi;
    };

    template <typename RealType>
    class TMathUtil : public TMathUtilConstants<RealType>
    {
    public:
        static inline bool IsNaN( const RealType Value )
        {
            return std::isnan( Value );
        }
        static inline bool IsFinite( const RealType Value )
        {
            return std::isfinite( Value );
        }
        static inline RealType Abs( const RealType Value )
        {
            return ( Value >= (RealType)0 ) ? Value : -Value;
        }
        static inline RealType Clamp( const RealType Value, const RealType ClampMin, const RealType ClampMax )
        {
            return ( Value < ClampMin ) ? ClampMin : ( ( Value > ClampMax ) ? ClampMax : Value );
        }
        static inline RealType Max( const RealType A, const RealType B )
        {
            return ( A >= B ) ? A : B;
        }
        static inline RealType Min( const RealType A, const RealType B )
        {
            return ( A <= B ) ? A : B;
        }
        static inline RealType Sqrt( const RealType Value )
        {
            return std::sqrt( Value );
        }
        static inline RealType ACos( const RealType Value )
        {
            return std::acos( Value );
        }
        static inline RealType Atan2( const RealType ValueY, const RealType ValueX )
        {
            return std::atan2( ValueY, ValueX );
        }

    private:
        TMathUtil() = delete;
    };

    using FMathf = TMathUtil<float>;
    using FMathd = TMathUtil<double>;
} // namespace Desert::Geometry

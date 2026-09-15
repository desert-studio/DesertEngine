#pragma once

// ROUNDING A REAL TO AN INTEGER, ONCE, FOR THE WHOLE ENGINE.
//
// `static_cast<int>( x + 0.5 )` was written fourteen times in this tree and it is not rounding. It is
// "add a half, then truncate TOWARD ZERO", so for a negative x it rounds the wrong way: -2.7 becomes -2
// where every reader expects -3, and -0.4 becomes 0 where the nearest integer is 0 — right by accident
// and wrong one tenth further out. Ten of the fourteen sites clamp to a non-negative range first and are
// therefore correct today; that is exactly what makes the form dangerous, because the four that do not
// look identical to the ten that do and nothing at the site says which kind it is.
//
// WHY THESE TWO FUNCTIONS AND NOT A COMMENT SAYING "MIND THE SIGN". The two questions the tree actually
// asks are different, and separating them is what removes the judgement from the call site:
//
//   * RoundToNearest  — a real to the nearest integer, for any sign, exactly. `std::lround` is one
//     instruction on both targets we ship (`fcvtas` on AArch64, `cvtss2si` on x86-64 SSE4), so the
//     correct version costs nothing: measured on clang 17 -O2 for AArch64, the add-and-truncate form and
//     this one are the same instruction count, and this one has no wrong half.
//   * QuantiseUnitToByte — a 0..1 fraction to an 8-bit channel. The clamp is part of the operation, not
//     something the caller is trusted to have done: every one of the ten sites this replaces wrote the
//     clamp out by hand, one of them with glm::clamp, one with a ternary, one with std::clamp.
//
// The 255 (not 256) and the round-to-nearest together are what make the mapping symmetric and its
// round trip exact: 0.0 -> 0, 1.0 -> 255, and byte/255.0f -> byte for every one of the 256 values.
// `CloudNoiseVolumeGenerator` had that written out in a comment as the reason not to truncate; the
// reason now lives in the function instead of in one of its callers.

#include <cmath>
#include <cstdint>

namespace Common::Math
{
    /// The nearest integer to @p value, halves away from zero. Correct for negative values, which is the
    /// whole point — see the note above.
    [[nodiscard]] inline long long RoundToNearest( double value ) noexcept
    {
        return std::lround( value );
    }

    /// @overload — float, so a float caller does not widen and narrow again.
    [[nodiscard]] inline long long RoundToNearest( float value ) noexcept
    {
        return std::lround( value );
    }

    /// A 0..1 fraction as an 8-bit channel. Values outside the range are CLAMPED, not wrapped: a
    /// quantiser that wraps turns one over-bright texel into a black one, which reads as a hole in the
    /// data rather than as a saturated sample. NaN clamps to 0 — the comparisons below are false for it,
    /// so it takes the `unit` branch and `lround` of a NaN is unspecified; the explicit test keeps the
    /// answer defined.
    [[nodiscard]] inline std::uint8_t QuantiseUnitToByte( float unit ) noexcept
    {
        if ( !( unit > 0.0F ) )
        { // false for NaN and for every value at or below zero
            return 0;
        }
        if ( unit >= 1.0F )
        {
            return 255;
        }
        return static_cast<std::uint8_t>( std::lround( unit * 255.0F ) );
    }
} // namespace Common::Math

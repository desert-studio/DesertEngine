#pragma once

#include <cstdint>

namespace Common
{
    /// `value` rounded up to a multiple of `alignment` (any alignment, not only powers of two); alignment 0 leaves
    /// the value as it is. The one rounding the GPU paths use for buffer offsets, ring slots and
    /// shader-binding-table strides.
    [[nodiscard]] constexpr uint64_t AlignUp( const uint64_t value, const uint64_t alignment ) noexcept
    {
        return alignment == 0 ? value : ( value + alignment - 1 ) / alignment * alignment;
    }
} // namespace Common

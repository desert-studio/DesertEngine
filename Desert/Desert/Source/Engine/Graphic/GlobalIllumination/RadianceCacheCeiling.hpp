#pragma once

#include <cstdint>

namespace Desert::Graphic::RadianceCache
{
    // RADIANCE_PROBE_MAX_CLIPMAPS: the radiance cache's clipmap ceiling (RadianceCache.hpp; LightingLayout.hpp sizes
    // the light grid's clipmap rows by it). Light on purpose: layout headers include it.
    inline constexpr uint32_t kMaxClipmaps = 6;
} // namespace Desert::Graphic::RadianceCache

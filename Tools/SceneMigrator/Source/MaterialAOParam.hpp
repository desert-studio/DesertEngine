#pragma once

#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string>

namespace Desert::Migration
{
    // ONE AMBIENT-OCCLUSION PARAMETER (MAT-AO-ONEHOME; UE's Ambient Occlusion input). StandardSurface and Toon
    // declared two — `AOStrength` multiplying the surface's AO and `OcclusionStrength` lerping the ORM map's R —
    // so one authored value was applied twice (0.5 read as 0.25). The templates now declare `AOStrength` alone,
    // applied once as the map's strength; a `.demat` stating `OcclusionStrength` states that same strength under
    // the retired name, and this step renames it. Both stated is two values for one input: refused, naming
    // @p source, never guessed. nullopt = the material states no `OcclusionStrength` (nothing to do); otherwise
    // the material's canonical text with the parameter renamed.
    Common::ResultStr<std::optional<std::string>> MaterialWithOneAOParam( const std::string& source,
                                                                          const std::string& text );
} // namespace Desert::Migration

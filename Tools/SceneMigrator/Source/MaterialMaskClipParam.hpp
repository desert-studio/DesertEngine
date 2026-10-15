#pragma once

#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string>

namespace Desert::Migration
{
    // THE FOLIAGE TEMPLATE IS MASKED (FOLIAGE-LOOK; UE's foliage material is BLEND_Masked). FoliageSurface.shader
    // declares `BlendMode Masked`, whose clip threshold is UE's Opacity Mask Clip Value under the one name the
    // shader parser requires (DShaderParser kSurfaceMaskClipParam), so the template's `AlphaCutoff` is gone: a
    // FoliageSurface `.demat` stating `AlphaCutoff` states that same threshold under the retired name, and this
    // step renames it. Only materials of that template (by its shader GUID): StandardSurface keeps AlphaCutoff.
    // Both stated is two values for one threshold: refused, naming @p source, never guessed. nullopt = nothing to
    // do; otherwise the material's canonical text with the parameter renamed.
    Common::ResultStr<std::optional<std::string>> FoliageMaterialWithMaskClipParam( const std::string& source,
                                                                                    const std::string& text );
} // namespace Desert::Migration

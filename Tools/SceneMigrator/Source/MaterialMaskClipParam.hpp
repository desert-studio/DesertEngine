#pragma once

#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string>

namespace Desert::Migration
{
    // CUT-OUT IS A BLEND MODE (MAT-MASKED; UE's BLEND_Masked with its Opacity Mask Clip Value). The templates no
    // longer discard by hand below an `AlphaCutoff`: a cut-out template declares `BlendMode Masked` and its
    // threshold under the one name the shader parser requires (DShaderParser kSurfaceMaskClipParam,
    // OpacityMaskClipValue), and StandardSurface is opaque only, its cut-out sibling StandardSurfaceMasked. A
    // `.demat` written before states the retired form, and this step re-states it:
    //   FoliageSurface + AlphaCutoff            -> OpacityMaskClipValue, same value;
    //   StandardSurface + AlphaCutoff > 0       -> the StandardSurfaceMasked template, OpacityMaskClipValue;
    //   StandardSurface, no cut (AlphaCutoff 0 or unstated) -> AlphaCutoff, OpacityChannel and OpacityTexture
    //                                              dropped (an opaque surface has no mask; MAT-MASKED dropped them
    //                                              from the committed materials by hand).
    // A material stating both AlphaCutoff and OpacityMaskClipValue states two values for one threshold: refused,
    // naming @p source, never guessed. nullopt = nothing to do; otherwise the material's canonical text.
    Common::ResultStr<std::optional<std::string>> MaterialWithMaskedTemplate( const std::string& source,
                                                                              const std::string& text );
} // namespace Desert::Migration

#pragma once

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // THE EXPOSURE OF A VIEW, as every pass that works on the exposed image reads it: x = the manual exposure
    // scale (ManualExposureScale of ManualExposureEV100 under the compensation), y = 1 when auto-exposure is on,
    // z = 2^AutoExposureBias (UE's ExposureCompensation).
    // Common/ViewExposure.glslh turns it and this frame's adapted luminance (the 1x1 AutoExposure.Adapted image)
    // into the exposure scale - the ONE formula the tonemap and the bloom setup both use, as UE's tonemapper and
    // bloom setup both read the one eye-adaptation buffer. A pass that thresholded unexposed light under
    // auto-exposure is what this replaced.
    inline glm::vec4 ViewExposureOf( const Core::PostProcessSettings& grade )
    {
        const float compensation = Core::AutoExposureCompensation( grade.AutoExposureBias );
        return { ManualExposureScale( grade.ManualExposureEV100, compensation ), grade.AutoExposure ? 1.0f : 0.0f,
                 compensation, 0.0f };
    }
} // namespace Desert::Graphic

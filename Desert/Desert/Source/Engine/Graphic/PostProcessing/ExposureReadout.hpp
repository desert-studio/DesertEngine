#pragma once

namespace Desert::Graphic
{
    // What a view's exposure was, for `state render` (render.exposure): the meter's read-back of FramesInFlight
    // frames ago under auto-exposure, the manual scale otherwise. EV100 = log2(L x 100 / 12.5) (K = 12.5,
    // AutoExposureRules LuminanceToEV100).
    struct ExposureReadout
    {
        bool  Auto          = false;
        float AdaptedEV100  = 0.0f; // the adapted luminance the frame used, as EV100
        float MeasuredEV100 = 0.0f; // that frame's percentile-trimmed average, before clamp and adaptation
        float Exposure      = 0.0f; // the linear multiplier the composite applied
        float LowPercent    = 0.0f; // the histogram tails metered with (%, clamped as UE clamps them)
        float HighPercent   = 0.0f;
    };
} // namespace Desert::Graphic

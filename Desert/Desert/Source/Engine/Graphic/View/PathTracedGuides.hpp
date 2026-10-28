#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>

namespace Desert::Graphic
{
    // THE PATHS' OWN SURFACE GUIDES of one frame, handed from the path tracer to an upscaler that denoises
    // (System::PathTracer::AddPasses -> FrameTransients::PathTracedGuides -> TemporalUpscalerInputs; UE's path
    // tracer hands its denoiser the albedo and normal it accumulated the same way, PathTracing.cpp). They are the
    // view's per-pixel SUMS over this frame's surface paths (Common/PathTracingDenoise.glslh, the one statement of
    // their layout), render extent, ViewTargetFormats::kPathTracingAccumulation:
    //   Albedo:   rgb = sum diffuse reflectance D_d, a = s (the pixel's surface paths; 0 = no surface)
    //   Normal:   xyz = sum first-surface world normal, w = sum camera distance (cm)
    //   Specular: rgb = sum split-sum specular reflectance D_s, a = sum roughness
    //   Hit:      x = sum w * t, y = sum w (t the first bounce's length, w its specular share)
    // The radiance the paths made is D_d * I_d + D_s * I_s per path, so these, divided by s, are exactly the
    // factors the colour was made with - glass looked through, sub-pixel averaged - where the raster G-buffer
    // shows another surface wherever the camera ray's first path-traced hit is not the rasterised one.
    // All invalid when the path tracer did not hand its paths raw to the upscaler this frame.
    struct PathTracedGuides
    {
        RDG::TextureRef Albedo;
        RDG::TextureRef Normal;
        RDG::TextureRef Specular;
        RDG::TextureRef Hit;

        [[nodiscard]] bool IsValid() const
        {
            return Albedo.IsValid() && Normal.IsValid() && Specular.IsValid() && Hit.IsValid();
        }
    };
} // namespace Desert::Graphic

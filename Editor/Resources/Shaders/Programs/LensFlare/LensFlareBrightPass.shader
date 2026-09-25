// DesertAsset {"Kind":"Shader","Guid":"df8242509db04fb5d5fcd44575359985","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LensFlareBrightPass"
{
    Compute
    {
        // The flare's SOURCE image: the HDR scene, thresholded, boxed down to quarter resolution, and
        // then down a mip chain — the same shader run once per mip, exactly as BloomDownsample is.
        //
        // Why a separate source rather than reading the scene directly in the features pass: every
        // feature there is a GATHER (a ghost rescales a neighbourhood, the streak walks 64 taps, the halo
        // sweeps a ring), and a small, very high-contrast sun disc point-sampled by a gather sparkles as
        // the camera moves. Boxing it down first is both the cheap answer and the correct one — a flare
        // is an out-of-focus image of the source, so the source it reflects is meant to be soft.
        //
        // Why a MIP CHAIN and not just one level: a ghost is a MAGNIFIED image of the source, and
        // magnifying one level 8x shows its texels — the first version of this drew the ghosts as visible
        // bilinear SQUARES, the exact "cheap sprite" look. The features pass therefore reads each ghost
        // at the mip its own magnification calls for, which is what an out-of-focus image actually is.
        //
        // The threshold is what makes this a SUN flare and not a smear of the whole sky: authored high
        // enough (Core::SceneSettings::LensFlareThreshold) that in a physical sky only the disc survives.
        // No radial window around the sun is applied, deliberately — a second bright source in frame
        // ought to flare too, and the threshold is the honest way to say what counts as bright.

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Output;

        PushConstant LensFlareBrightPush
        {
            int   u_SrcMip;        // mip of u_Source to read (ignored on the first pass)
            int   u_FirstPass;     // 1 = u_Source is the HDR scene and the threshold applies
            float u_Threshold;     // HDR luminance below this contributes nothing
            float u_MaxBrightness; // cap on extracted energy — one blown pixel must not own the flare
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 size  = imageSize(u_Output);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            vec2 uv    = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 texel = vec2(1.0f, 1.0f) / vec2(size);

            // Four bilinear taps at HALF a destination texel — a 4x4 tent of the level below for four
            // fetches. Clamped because the engine-wide sampler is REPEAT and a tap off one edge would
            // otherwise pull the opposite edge's content in.
            //
            // The half is load-bearing and was learned the hard way. At a quarter of a destination texel
            // the taps land exactly on SOURCE TEXEL CENTRES, where bilinear interpolation degenerates to
            // point sampling and the whole filter collapses to a plain 2x2 box. A box kernel has square
            // support, so applying it down a mip chain turns a near-point source into a SQUARE — and the
            // ghosts, which are magnified images of exactly that, came out as rounded squares. At half a
            // destination texel each tap sits on a source texel boundary, so it genuinely interpolates
            // two texels per axis and the filter is a tent, which converges toward a gaussian instead.
            vec2 o   = texel * 0.5f;
            vec2 lo  = texel * 0.5f;
            vec2 hi  = vec2(1.0f, 1.0f) - lo;
            float m  = float(u_SrcMip);
            vec3 colour = textureLod(u_Source, clamp(uv + vec2(-o.x, -o.y), lo, hi), m).rgb +
                          textureLod(u_Source, clamp(uv + vec2( o.x, -o.y), lo, hi), m).rgb +
                          textureLod(u_Source, clamp(uv + vec2(-o.x,  o.y), lo, hi), m).rgb +
                          textureLod(u_Source, clamp(uv + vec2( o.x,  o.y), lo, hi), m).rgb;
            colour *= 0.25f;

            if (u_FirstPass == 1)
            {
                // Hue-preserving bright pass — the same form BloomDownsample and LightShaftMask use, so
                // all three effects agree about what "bright" means. Applied only once: the deeper mips
                // are averages of already-thresholded energy, and thresholding them again would eat the
                // soft skirt that makes a magnified ghost read as out of focus rather than as a stamp.
                float brightness   = max(colour.r, max(colour.g, colour.b));
                float contribution = max(brightness - u_Threshold, 0.0f) / max(brightness, 1e-4f);
                colour             = min(colour * contribution, vec3(u_MaxBrightness));
            }

            imageStore(u_Output, coord, vec4(colour, 1.0f));
        }
    }
}

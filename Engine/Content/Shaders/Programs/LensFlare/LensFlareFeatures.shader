// DesertAsset {"Kind":"Shader","Guid":"757b690db46cc53baf280bebdb8d637c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LensFlareFeatures"
{
    Compute
    {
        // The lens flare itself: ghosts along the sun->centre axis, a halo ring about the sun, and an
        // anamorphic streak — every one of them an IMAGE of the flare source, gathered, never a drawn
        // sprite. Placement and weighting come from Common/LensFlare.glslh, which the LensFlare tests
        // compile as C++; this file only does the sampling.
        //
        // Nothing here is a look. Count, spacing, sizes, tints, halo radius, streak axis and length and
        // the chromatic shift are all authored on Core::SceneSettings ("Lens Flare") and arrive below as
        // push constants. Adding a differently-coloured ghost train is a scene edit, not a shader edit.

        #include <Common/LensFlare.glslh>

        Uniform(0) sampler2D u_FlareSource; // quarter-res thresholded scene (LensFlareBrightPass)
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Flare;

        PushConstant LensFlareFeaturesPush
        {
            vec4 u_SunUvHalo;   // xy = sun screen uv, z = halo intensity, w = halo radius
            vec4 u_GhostParams; // x = count, y = spacing, z = size near, w = size far
            vec4 u_TintInner;   // xyz = inner ghost tint, w = chromatic shift
            vec4 u_TintOuter;   // xyz = outer ghost tint, w = screen pixels per mip-0 source texel
            vec4 u_Streak;      // x = intensity, y = length, z = axis.x, w = axis.y
        };

        LocalSize(16, 16, 1);

        // Quality constants of the MECHANISM, not content: how finely the streak is integrated and the
        // ceiling on an authored ghost count. Raising either costs time and changes nothing about the
        // look an artist authored.
        //
        // 64 taps is not arbitrary. The streak is a convolution of the source with the axis, and the sum
        // only reads as a line if consecutive taps OVERLAP the source. The sun disc is ~7 texels wide in
        // this quarter-resolution source, i.e. ~0.02 in uv; at 16 taps over a 0.35 half-length the step
        // was 0.047 and the streak came out as a row of separate squares — 16 copies of the sun, which is
        // exactly the "cheap sprite" this effect must never look like. 64 taps puts the step under the
        // disc's own width and the copies merge into one continuous smear.
        const float kStreakTaps = 64.0f;
        const int   kMaxGhosts  = 8;

        // Blur floor. Even at magnification 1 a flare is an out-of-focus image, and reading mip 0 makes
        // the halo a one-texel-thin rainbow wire instead of a ring.
        const float kMinSourceLod = 1.0f;

        // Sampling the source at mip @p lod. Clamped to [0,1] because the engine-wide sampler is REPEAT
        // and a ghost reaching past the frame would otherwise wrap the opposite edge's content in.
        vec3 SampleSourceLod(vec2 uv, float lod)
        {
            return textureLod(u_FlareSource, clamp(uv, vec2(0.0f, 0.0f), vec2(1.0f, 1.0f)),
                              max(lod, kMinSourceLod)).rgb;
        }

        // The mip a feature magnified by @p scale should read: magnifying a level shows its texels, so
        // step down a level per doubling. This is the whole reason the source has a chain.
        //
        // @p sourceToScreen is how many screen pixels one mip-0 source texel already covers — the source
        // is a QUARTER-resolution image, so a ghost authored at scale 3 is magnified twelve times on
        // screen, not three. Leaving that factor out is what left visible bilinear squares in the ghosts
        // after the mip chain was added: the lod was two levels too sharp for the magnification.
        float SourceLodForScale(float scale, float sourceToScreen)
        {
            return log2(max(scale * sourceToScreen, 1.0f));
        }

        void main()
        {
            ivec2 size  = imageSize(u_Flare);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            vec2  uv     = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            float aspect = float(size.x) / max(float(size.y), 1.0f);

            vec2  sunUv      = vec2(u_SunUvHalo.x, u_SunUvHalo.y);
            float ghostCount = u_GhostParams.x;
            float chroma     = u_TintInner.w;
            vec3  tintInner  = vec3(u_TintInner.x, u_TintInner.y, u_TintInner.z);
            vec3  tintOuter  = vec3(u_TintOuter.x, u_TintOuter.y, u_TintOuter.z);
            float srcToScreen = max(u_TintOuter.w, 1.0f);

            vec3 flare = vec3(0.0f, 0.0f, 0.0f);

            // --- Ghosts ------------------------------------------------------------------------------
            // Each ghost is the source rescaled about its own centre. The three channels are read at
            // slightly different scales, so a ghost carries the coloured fringe a real coated element
            // leaves — the fringe is the SCENE dispersed, not a tint painted on.
            for (int i = 0; i < kMaxGhosts; ++i)
            {
                float index = float(i);
                if (index >= ghostCount)
                    break;

                vec2  center = LensFlareGhostCenter(sunUv, index, u_GhostParams.y);
                float scale  = LensFlareGhostScale(index, ghostCount, u_GhostParams.z, u_GhostParams.w);
                vec3  tint   = LensFlareGhostTint(index, ghostCount, tintInner, tintOuter);

                float spread = 1.0f + chroma * 0.08f;
                vec2  srcR   = LensFlareGhostSourceUv(uv, center, scale * spread, sunUv);
                vec2  srcG   = LensFlareGhostSourceUv(uv, center, scale, sunUv);
                vec2  srcB   = LensFlareGhostSourceUv(uv, center, scale / spread, sunUv);

                float weight = LensFlareGhostWeight(srcG, uv, center, scale, aspect);
                if (weight <= 0.0f)
                    continue;

                float lod  = SourceLodForScale(scale, srcToScreen);
                vec3 ghost = vec3(SampleSourceLod(srcR, lod).r, SampleSourceLod(srcG, lod).g,
                                  SampleSourceLod(srcB, lod).b);
                flare += ghost * tint * weight;
            }

            // --- Halo --------------------------------------------------------------------------------
            // The ring reads the source at the pixel's own bearing from the sun, pinned to the halo
            // radius: a bright arc beside the sun brightens that arc of the ring and nothing else.
            {
                float ringWeight = LensFlareHaloWeight(uv, sunUv, u_SunUvHalo.w, aspect);
                if (ringWeight > 0.0f)
                {
                    // The channel separation here is an ABSOLUTE offset, not a fraction of the halo
                    // radius. Scaling it by the radius (0.32) made the three channels miss each other by
                    // twenty times the sun's own width, and the ring came out as three separate
                    // saturated circles — a rainbow wire, the fixed-circle look this must never have.
                    // The fringe has to stay inside the feature it fringes.
                    float shift = chroma * 0.012f;
                    vec2  hR    = LensFlareHaloSourceUv(uv, sunUv, u_SunUvHalo.w + shift, aspect);
                    vec2  hG    = LensFlareHaloSourceUv(uv, sunUv, u_SunUvHalo.w, aspect);
                    vec2  hB    = LensFlareHaloSourceUv(uv, sunUv, u_SunUvHalo.w - shift, aspect);

                    // A halo is the most defocused feature of the lot — read it well down the chain so
                    // the ring has real width instead of tracing the disc's own sharp edge.
                    float lod  = kMinSourceLod + 1.0f;
                    vec3  halo = vec3(SampleSourceLod(hR, lod).r, SampleSourceLod(hG, lod).g,
                                      SampleSourceLod(hB, lod).b);
                    flare += halo * ringWeight * u_SunUvHalo.z;
                }
            }

            // --- Anamorphic streak -------------------------------------------------------------------
            // A gather along the authored axis: the streak's profile is the source's profile smeared,
            // so an occluded sun shortens and dims its own streak with no extra code.
            if (u_Streak.x > 0.0f && u_Streak.y > 0.0f)
            {
                vec2  axis     = vec2(u_Streak.z, u_Streak.w);
                vec3  streak   = vec3(0.0f, 0.0f, 0.0f);
                float weightSum = 0.0f;

                for (float k = 0.0f; k < kStreakTaps; k += 1.0f)
                {
                    float offset = LensFlareStreakTapOffset(k, kStreakTaps);
                    float w      = LensFlareStreakTapWeight(offset);
                    vec2  tapUv  = LensFlareStreakTapUv(uv, axis, offset, u_Streak.y, aspect);

                    streak += SampleSourceLod(tapUv, kMinSourceLod) * w;
                    weightSum += w;
                }

                flare += (streak / max(weightSum, 1e-4f)) * u_Streak.x;
            }

            imageStore(u_Flare, coord, vec4(flare, 1.0f));
        }
    }
}

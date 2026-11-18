// DesertAsset {"Kind":"Shader","Guid":"725394a9dea88871b2f1e4b25639df77","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SceneComposite"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(1) sampler LinearRepeatSampler;
        Uniform(7) sampler LinearClampSampler;
        Uniform(2) texture2D GeometryTexture;
        #define GeometryTexture sampler2D(GeometryTexture, LinearRepeatSampler)
        Uniform(3) texture2D BloomTexture;
        #define BloomTexture sampler2D(BloomTexture, LinearClampSampler)
        Uniform(4) texture2D AverageLuminanceTexture;      // 1x1 adapted luminance (eye adaptation)
        #define AverageLuminanceTexture sampler2D(AverageLuminanceTexture, LinearRepeatSampler)
        Uniform(5) texture2D LightShaftTexture; // radial sun streaks (LightShaftRenderer), half res
        #define LightShaftTexture sampler2D(LightShaftTexture, LinearClampSampler)
        Uniform(6) texture2D LensFlareTexture;  // ghosts/halo/streak (LensFlareRenderer), quarter res
        #define LensFlareTexture sampler2D(LensFlareTexture, LinearClampSampler)
        // Local exposure (LocalExposureRenderer): the bilateral grid (sum log luminance, count) and the blurred 1/32
        // log luminance; System.Black3D / System.Black with mode 0 when the passes did not run.
        Uniform(8) texture3D LumBilateralGrid;
        #define LumBilateralGrid sampler3D(LumBilateralGrid, LinearClampSampler)
        Uniform(9) texture2D BlurredLogLuminance;
        #define BlurredLogLuminance sampler2D(BlurredLogLuminance, LinearClampSampler)
        // Local exposure, Fusion: the blended pyramid's level 0 (fused display value), full resolution; System.Black
        // when the fusion did not run (then LocalExposureMethod is 0 and it is not read).
        Uniform(10) texture2D ExposureFusionTexture;
        #define ExposureFusionTexture sampler2D(ExposureFusionTexture, LinearClampSampler)
        // THE GRADE OF THE VIEW (UE CombineLUTs): white balance, colour grading, tone curve and output device
        // baked into a 32^3 table this frame for THIS target's output (CombineLUTs.shader: SDR, or extended sRGB at the
        // HDR headroom), indexed by UE's log encoding of the exposed colour.
        Uniform(11) texture3D ColorGradingLUT;
        #define ColorGradingLUT sampler3D(ColorGradingLUT, LinearClampSampler)
        Out(0) vec4 oColor;

        Uniform(0) TonemapUB
        {
            float Exposure;
            float BloomIntensity;
            float ExposureKey;          // middle-grey target for auto-exposure
            float AutoExposureEnabled;  // > 0.5 -> use measured luminance instead of manual exposure
            float ChromaticBloom;       // lens dispersion strength on the bloom halo (0 = off)
            float TonemapOperator;      // Core::TonemapOperator (0 ACES, 1 Reinhard, 2 Filmic); 3 = buffer visualization
            float InversePreExposure;       // 1 / ViewFrame::PreExposure (the std140 slot before the vec4s)
            vec4  LightShaftTintIntensity; // rgb = the sun light's Bloom Tint, a = Bloom Scale x screen fade
            vec4  LensFlareTintIntensity;  // rgb = the lens's Tint, a = Intensity x screen fade
            vec4  LocalExposureContrast;   // highlight contrast, shadow contrast, detail strength, blurred blend
            vec4  LocalExposureThreshold;  // highlight / shadow threshold (EV), highlight / shadow strength
            vec4  LocalExposureUV;         // grid UV scale xy, blurred UV scale xy
            vec4  LocalExposureSlice;      // log2 middle grey (exposed), slice log min, 1 / EV per slice, mode
            float LocalExposureMethod;     // 1: Fusion (ExposureFusionTexture), 0: Bilateral (grid + blur)
            // THE LENS (LENS-POST, TonemapRenderer::FillLens); all zero = off and the picture is untouched.
            vec4  LensFringeVignette;      // aberration scale R, scale G, start offset; vignette intensity
            vec4  LensGrain;               // grain intensity, grain texel size (pixels), frame seed, 0
            vec4  LensPanini;              // Panini D, Panini S, tan(half FOV x), tan(half FOV y)
        };

        // PANINI (UE 5 PaniniProjection.usf PaniniProjection, adapted: GLSL). OM: a direction on the unit-distance
        // plane; the result: where it lands on the Panini plane of distance d, squeezed vertically by s.
        vec2 PaniniProjection(vec2 OM, float d, float s)
        {
            float paniniDirectionXZInvLength = inversesqrt(1.0 + OM.x * OM.x);
            float sinPhi   = OM.x * paniniDirectionXZInvLength;
            float tanTheta = OM.y * paniniDirectionXZInvLength;
            float cosPhi   = sqrt(1.0 - sinPhi * sinPhi);
            float S        = (d + 1.0) / (d + cosPhi);
            return S * vec2(sinPhi, mix(tanTheta, tanTheta / cosPhi, s));
        }

        // The source UV a Panini output pixel reads (UE's upscale pass, screen fit 1): the pixel's direction through
        // the Panini map, scaled so the horizontal edge stays the edge. d = 0, s = 0 is the identity.
        vec2 PaniniSourceUV(vec2 uv)
        {
            vec2  tanHalfFov = LensPanini.zw;
            vec2  OM         = (uv * 2.0 - 1.0) * tanHalfFov;
            vec2  panini     = PaniniProjection(OM, LensPanini.x, LensPanini.y);
            float edge       = PaniniProjection(vec2(tanHalfFov.x, 0.0), LensPanini.x, 0.0).x;
            vec2  screen     = panini * (tanHalfFov.x / max(edge, 1e-6)) / tanHalfFov;
            return clamp(screen * 0.5 + 0.5, 0.0, 1.0);
        }

        // The grain's white noise per cell and frame: an integer hash (PCG3D, Jarzynski & Olano 2020).
        uvec3 LensPcg3d(uvec3 v)
        {
            v = v * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            return v;
        }

        #include <Common/FilmToneMap.glslh>
        #include <Common/LocalExposure.glslh>

        // THE DISPLAY TRANSFER: the sRGB OETF (IEC 61966-2-1), the curve the display decodes with.
        // Ported from UE 5.8 Engine/Shaders/Private/GammaCorrectionCommon.ush:16-18 (LinearToSrgbBranchless),
        // adapted: GLSL, input clamped to [0,1] (the tonemappers' output range). It was pow(1/Gamma) with a scene's
        // Gamma 2.2: a pure power encode that the display's sRGB decode does not invert lifted every value below
        // ~0.04 linear - 24/255 drew where 17/255 belongs, so shade under the grass and in the barn's corners read
        // greyed. UE encodes with this curve for its default output device (r.HDR.Display.OutputDevice 0, sRGB);
        // the transfer is the display's, not a per-scene look, so it is no longer a PostProcess setting.
        vec3 LinearToSrgb(vec3 linearColor)
        {
            const vec3 c = clamp(linearColor, 0.0, 1.0);
            return min(c * 12.92, pow(max(c, vec3(0.00313067)), vec3(1.0 / 2.4)) * 1.055 - 0.055);
        }

        void main()
        {
            // THE BUFFER VISUALIZATION (operator 2, MaterialTonemap::Params::VisualizeBuffer; UE VMI_VisualizeBuffer):
            // the composite wrote a stored value or a heat colour pre-exposed, so undoing the pre-exposure leaves it
            // as authored - no eye adaptation, exposure, effects or curve, which turned it black in a lit scene.
            if (TonemapOperator > 2.5)
            {
                vec3 stored = clamp(texture(GeometryTexture, v_TexCoord).rgb * InversePreExposure, 0.0, 1.0);
                oColor      = vec4(LinearToSrgb(stored), 1.0);
                return;
            }

            // Auto-exposure: scale so the adapted average luminance maps to the key value; else manual exposure.
            float exposure = Exposure;
            if (AutoExposureEnabled > 0.5)
            {
                // .g: the adapted average over 2^compensation (AEAverage.shader) - UE's exposure compensation.
                float adaptedLum = texture(AverageLuminanceTexture, vec2(0.5)).g;
                exposure = ExposureKey / max(adaptedLum, 1e-4);
            }

            // THE LENS'S GEOMETRY (LENS-POST): with Panini on every input below is read through the one remapped UV,
            // so the scene and its bloom, shafts and flare stay registered; off, uv is v_TexCoord exactly.
            vec2 uv = v_TexCoord;
            if (LensPanini.x > 0.0 || LensPanini.y != 0.0)
            {
                uv = PaniniSourceUV(v_TexCoord);
            }

            vec3 scene = texture(GeometryTexture, uv).rgb;
            // CHROMATIC ABERRATION (UE PostProcessTonemap.usf SceneFringe: the lens position minus its sign times
            // the distance past the start offset times the channel's scale): red and green read nearer the centre,
            // so they are magnified outward over blue. The sampler repeats, so the UVs are clamped.
            if (LensFringeVignette.x > 0.0)
            {
                vec2 lens = uv * 2.0 - 1.0;
                vec2 past = sign(lens) * clamp(abs(lens) - LensFringeVignette.z, 0.0, 1.0);
                vec2 uvR  = clamp((lens - past * LensFringeVignette.x) * 0.5 + 0.5, 0.0, 1.0);
                vec2 uvG  = clamp((lens - past * LensFringeVignette.y) * 0.5 + 0.5, 0.0, 1.0);
                scene.r   = texture(GeometryTexture, uvR).r;
                scene.g   = texture(GeometryTexture, uvG).g;
            }

            // Bloom. With lens dispersion on, sample the bloom per-channel along a radial offset (R pushed outward,
            // B inward), proportional to distance from the screen centre -> a rainbow fringe / glare around bright
            // sources. The global sampler is REPEAT, so clamp the offset UVs to [0,1] (else bright content wraps).
            vec3 bloom;
            if (ChromaticBloom > 0.0001)
            {
                vec2  dir   = uv - vec2(0.5);
                vec2  off   = dir * (0.012 * ChromaticBloom);
                bloom.r = texture(BloomTexture, clamp(uv + off, 0.0, 1.0)).r;
                bloom.g = texture(BloomTexture, uv).g;
                bloom.b = texture(BloomTexture, clamp(uv - off, 0.0, 1.0)).b;
            }
            else
            {
                bloom = texture(BloomTexture, uv).rgb;
            }
            bloom *= BloomIntensity;

            // Light shafts: additive HDR streaks toward the sun, BEFORE the tonemap so a strong shaft
            // rolls off through the same operator everything else does instead of clipping. The
            // intensity carries the screen-edge fade, so a sun leaving the view takes its streaks with
            // it; when the effect is off the intensity is exactly zero and the (stale) texture is inert.
            vec3 shafts = texture(LightShaftTexture, uv).rgb *
                          (LightShaftTintIntensity.rgb * LightShaftTintIntensity.a);

            // Lens flare: added in HDR alongside bloom and the shafts, before the tonemap, so a bright
            // ghost rolls off through the same operator instead of clipping to a flat disc. The intensity
            // carries the sun's screen-edge fade and is exactly zero whenever the flare pass did not run,
            // which is what makes the (stale) texture inert — the bloom image's contract.
            vec3 flare = texture(LensFlareTexture, uv).rgb *
                         (LensFlareTintIntensity.rgb * LensFlareTintIntensity.a);

            // LOCAL EXPOSURE (UE PostProcessTonemap.usf:411-423): per pixel, from the un-exposed luminance, the grid's
            // base layer and the blurred log luminance. It scales the SCENE term only (UE :523 vs :526): the bloom
            // carries its own from its setup. Mode (LocalExposureSlice.w): 0 off, 1 on, 2 on + visualised,
            // 3 visualised at identity (no passes ran: exposure 1 everywhere).
            float localExposure = 1.0;
            const float leMode  = LocalExposureSlice.w;
            if (leMode > 0.5 && leMode < 2.5 && LocalExposureMethod > 0.5)
            {
                // FUSION (UE 5.4 PostProcessTonemap.usf:424-435): the fused display value back through the inverse
                // fusion curve, over this pixel's own exposed luminance.
                float fused = textureLod(ExposureFusionTexture, uv, 0.0).r;
                float lum   = LocalExposureLuminance(scene) * InversePreExposure * exposure;
                localExposure = LocalExposureFusionExposure(lum, fused, abs(TonemapOperator - 1.0) < 0.5);
            }
            else if (leMode > 0.5 && leMode < 2.5)
            {
                float logLum      = log2(max(LocalExposureLuminance(scene) * InversePreExposure, 1.0e-10));
                float logExposure = log2(max(exposure, 1.0e-10));
                vec2  cell    = textureLod(LumBilateralGrid, vec3(uv * LocalExposureUV.xy,
                                           LocalExposureGridW(logLum, LocalExposureSlice.y, LocalExposureSlice.z)), 0.0).xy;
                float blurred = textureLod(BlurredLogLuminance, uv * LocalExposureUV.zw, 0.0).r;
                float base    = LocalExposureBaseLogLuminance(cell, blurred, LocalExposureContrast.w, logExposure);
                localExposure = exp2(LocalExposureLogExposure(logLum + logExposure, base, LocalExposureSlice.x,
                                                              LocalExposureContrast, LocalExposureThreshold));
            }

            // Scene, bloom, shafts and flare are all made from the pre-exposed scene colour: one division undoes it.
            vec3 color = (scene * localExposure + bloom + shafts + flare) * (exposure * InversePreExposure);

            // VIGNETTE (UE PostProcessTonemap ComputeVignetteMask, VignetteSpace from the vertex shader, adapted:
            // per pixel): the cosine-fourth law of natural vignetting in a space that keeps it a circle with the
            // same corner falloff at any aspect, over the linear colour before the grade, as UE.
            if (LensFringeVignette.w > 0.0)
            {
                vec2  size   = vec2(textureSize(GeometryTexture, 0));
                float aspect = size.y / size.x;
                vec2  circle = (v_TexCoord * 2.0 - 1.0) * vec2(1.0, aspect) * (sqrt(2.0) / sqrt(1.0 + aspect * aspect));
                circle      *= LensFringeVignette.w;
                float cos2   = 1.0 / (dot(circle, circle) + 1.0);
                color       *= cos2 * cos2;
            }

            // FILM GRAIN: zero-mean white noise per grain cell and frame, multiplying the exposed linear colour, so
            // the grain scales with the signal (film) and leaves the mean brightness where it was. UE 4's
            // GrainIntensity multiplied by the noise itself (mean 0.5) and so darkened the picture by half the
            // intensity; the zero-mean form does not.
            if (LensGrain.x > 0.0)
            {
                uvec3 h     = LensPcg3d(uvec3(uvec2(gl_FragCoord.xy / LensGrain.y), uint(LensGrain.z)));
                float noise = float(h.x >> 8u) * (2.0 / 16777216.0) - 1.0;
                color      *= max(1.0 + LensGrain.x * noise, 0.0);
            }

            // THE GRADE (UE CombineLUTs, PostProcessTonemap.usf ColorLookupTable): white balance, colour grading,
            // the operator and the output transform are the baked table's, bound for this target's output (the SDR
            // output, the HDR output at its headroom, or the HDR rendition of a capture).
            const float lutSize = float(textureSize(ColorGradingLUT, 0).x);
            vec3 lutUVW = FilmLinToLog(color + FilmLogToLin(vec3(0.0))) * ((lutSize - 1.0) / lutSize)
                        + 0.5 / lutSize;
            vec3 mapped = textureLod(ColorGradingLUT, lutUVW, 0.0).rgb;

            // The Local Exposure view mode (UE VisualizeLocalExposure, "Local Exposure"): the picture's own grey
            // tinted green where local exposure lifts it, red where it pulls it down, saturating at +-2 EV.
            if (leMode > 1.5)
            {
                float grey = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
                float t    = clamp(log2(localExposure) * 0.5, -1.0, 1.0);
                vec3  tint = t > 0.0 ? vec3(0.15, 0.95, 0.25) : vec3(0.95, 0.2, 0.15);
                mapped     = mix(vec3(grey), tint, abs(t) * 0.8);
            }

            // No dither here: FXAA / SMAA and the UI composite still change this colour, so the quantisation
            // dither is the display encode's, the pass that writes the back buffer (SwapchainBlit.shader).
            oColor = vec4(mapped, 1.0);
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord; 

        void main()
        {
            v_TexCoord = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}

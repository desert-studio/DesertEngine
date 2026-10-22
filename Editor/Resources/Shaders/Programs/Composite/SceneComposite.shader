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
        Out(0) vec4 oColor;

        Uniform(0) TonemapUB
        {
            float Exposure;
            float BloomIntensity;
            float ExposureKey;          // middle-grey target for auto-exposure
            float AutoExposureEnabled;  // > 0.5 -> use measured luminance instead of manual exposure
            float ChromaticBloom;       // lens dispersion strength on the bloom halo (0 = off)
            float WhitePoint;           // REINHARD ONLY: the luminance that maps to pure white (see below)
            float TonemapOperator;      // Core::TonemapOperator: < 0.5 = ACES, otherwise extended Reinhard
            float InversePreExposure;       // 1 / ViewFrame::PreExposure (the std140 slot before the vec4s)
            vec4  LightShaftTintIntensity; // rgb = the sun light's Bloom Tint, a = Bloom Scale x screen fade
            vec4  LensFlareTintIntensity;  // rgb = the lens's Tint, a = Intensity x screen fade
        };

        #include <Common/TonemapACES.glslh>

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

        // Extended Reinhard, on luminance, preserving the colour's ratio to it.
        // see: "Photographic Tone Reproduction for Digital Images", eq. 4
        //
        // pureWhite is a UNIFORM and not a constant, and that is the whole point of it:
        //     L' = L * (1 + L / W^2) / (1 + L)
        // at W = 1 reduces ALGEBRAICALLY to L' = L. The operator was once hard-coded to W = 1, so this
        // pass tonemapped nothing — every luminance above 1 walked through untouched and was clipped by
        // the 8-bit store, which is what flattened bright content into paper silhouettes: a highlight lit
        // to 2.0 and one lit to 6.0 both wrote 255 and lost every gradient between them.
        vec3 TonemapReinhard(vec3 color, float whitePoint)
        {
            float pureWhite       = max(whitePoint, 1.0);
            float luminance       = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float mappedLuminance = (luminance * (1.0 + luminance / (pureWhite * pureWhite))) / (1.0 + luminance);
            return (mappedLuminance / (luminance + 0.0001)) * color;
        }

        void main()
        {
            // Auto-exposure: scale so the adapted average luminance maps to the key value; else manual exposure.
            float exposure = Exposure;
            if (AutoExposureEnabled > 0.5)
            {
                float adaptedLum = texture(AverageLuminanceTexture, vec2(0.5)).r;
                exposure = ExposureKey / max(adaptedLum, 1e-4);
            }

            vec3 scene = texture(GeometryTexture, v_TexCoord).rgb;

            // Bloom. With lens dispersion on, sample the bloom per-channel along a radial offset (R pushed outward,
            // B inward), proportional to distance from the screen centre -> a rainbow fringe / glare around bright
            // sources. The global sampler is REPEAT, so clamp the offset UVs to [0,1] (else bright content wraps).
            vec3 bloom;
            if (ChromaticBloom > 0.0001)
            {
                vec2  dir   = v_TexCoord - vec2(0.5);
                vec2  off   = dir * (0.012 * ChromaticBloom);
                bloom.r = texture(BloomTexture, clamp(v_TexCoord + off, 0.0, 1.0)).r;
                bloom.g = texture(BloomTexture, v_TexCoord).g;
                bloom.b = texture(BloomTexture, clamp(v_TexCoord - off, 0.0, 1.0)).b;
            }
            else
            {
                bloom = texture(BloomTexture, v_TexCoord).rgb;
            }
            bloom *= BloomIntensity;

            // Light shafts: additive HDR streaks toward the sun, BEFORE the tonemap so a strong shaft
            // rolls off through the same operator everything else does instead of clipping. The
            // intensity carries the screen-edge fade, so a sun leaving the view takes its streaks with
            // it; when the effect is off the intensity is exactly zero and the (stale) texture is inert.
            vec3 shafts = texture(LightShaftTexture, v_TexCoord).rgb *
                          (LightShaftTintIntensity.rgb * LightShaftTintIntensity.a);

            // Lens flare: added in HDR alongside bloom and the shafts, before the tonemap, so a bright
            // ghost rolls off through the same operator instead of clipping to a flat disc. The intensity
            // carries the sun's screen-edge fade and is exactly zero whenever the flare pass did not run,
            // which is what makes the (stale) texture inert — the bloom image's contract.
            vec3 flare = texture(LensFlareTexture, v_TexCoord).rgb *
                         (LensFlareTintIntensity.rgb * LensFlareTintIntensity.a);

            // Scene, bloom, shafts and flare are all made from the pre-exposed scene colour: one division undoes it.
            vec3 color = (scene + bloom + shafts + flare) * (exposure * InversePreExposure);

        	// The scene's chosen operator. Both branches return LINEAR colour — the display encode below is
        	// the display transfer function and belongs to neither of them.
        	vec3 mappedColor = (TonemapOperator < 0.5) ? TonemapACES(color)
        	                                             : TonemapReinhard(color, WhitePoint);

        	vec3 mapped = LinearToSrgb(mappedColor);

        	// Ordered dithering on the FINAL 8-bit output to break gradient banding. Without it, smooth gradients
        	// (esp. the procedural sky) quantize into 8-bit steps; under camera motion those steps sweep across the
        	// screen and read as a "lighter/darker" shimmer. Keyed on gl_FragCoord so the pattern is stationary on
        	// screen (never crawls). +/- ~0.5 LSB is enough to randomize the rounding and is invisible otherwise.
        	float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        	mapped += (dither - 0.5) / 255.0;

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

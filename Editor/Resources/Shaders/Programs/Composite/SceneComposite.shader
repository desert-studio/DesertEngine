// DesertAsset {"Kind":"Shader","Guid":"725394a9dea88871b2f1e4b25639df77","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SceneComposite"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;
        Uniform(2) sampler2D u_GeometryTexture;
        Uniform(3) sampler2D u_BloomTexture;
        Uniform(4) sampler2D u_AvgLuminance;      // 1x1 adapted luminance (eye adaptation)
        Uniform(5) sampler2D u_LightShaftTexture; // radial sun streaks (LightShaftRenderer), half res
        Uniform(6) sampler2D u_LensFlareTexture;  // ghosts/halo/streak (LensFlareRenderer), quarter res
        // The view's whole grade - white balance, colour correction, tone curve, display transfer - baked into a 32^3
        // table once per change (CombineLUTs.shader, UE's CombineLUTs), indexed by the log-encoded exposed colour.
        Uniform(7) sampler3D u_ColorGradingLUT;
        Out(0) vec4 oColor;

        Uniform(0) TonemapUB
        {
            vec4  u_ExposureParams; // Graphic::ViewExposureOf: manual exposure, auto on, 2^AutoExposureBias
            float u_BloomIntensity;
            float u_ChromaticBloom;       // lens dispersion strength on the bloom halo (0 = off)
            vec4  u_LightShaftTintIntensity; // rgb = the sun light's Bloom Tint, a = Bloom Scale x screen fade
            vec4  u_LensFlareTintIntensity;  // rgb = the lens's Tint, a = Intensity x screen fade
        };

        #include <Common/FilmToneMap.glslh>
        #include <Common/ViewExposure.glslh>

        // UE's ColorLookupTable (PostProcessTonemap.usf:190-211) for the log-encoded table: the exposed colour is
        // offset by LogToLin(0) so black lands on the first texel, encoded, and sampled at texel centres.
        vec3 ColorLookupTable(vec3 linearColor)
        {
            float lutSize  = float(textureSize(u_ColorGradingLUT, 0).x);
            vec3  encoded  = FilmLinToLog(linearColor + FilmLogToLin(vec3(0.0)));
            vec3  uvw      = encoded * ((lutSize - 1.0) / lutSize) + vec3(0.5 / lutSize);
            return texture(u_ColorGradingLUT, uvw).rgb;
        }

        void main()
        {
            // The one exposure of the view (ViewExposure.glslh), the same the bloom setup thresholded with.
            float exposure = ViewExposureScale(u_ExposureParams, texture(u_AvgLuminance, vec2(0.5)).r);

            vec3 scene = texture(u_GeometryTexture, v_TexCoord).rgb;

            // Bloom. With lens dispersion on, sample the bloom per-channel along a radial offset (R pushed outward,
            // B inward), proportional to distance from the screen centre -> a rainbow fringe / glare around bright
            // sources. The global sampler is REPEAT, so clamp the offset UVs to [0,1] (else bright content wraps).
            vec3 bloom;
            if (u_ChromaticBloom > 0.0001)
            {
                vec2  dir   = v_TexCoord - vec2(0.5);
                vec2  off   = dir * (0.012 * u_ChromaticBloom);
                bloom.r = texture(u_BloomTexture, clamp(v_TexCoord + off, 0.0, 1.0)).r;
                bloom.g = texture(u_BloomTexture, v_TexCoord).g;
                bloom.b = texture(u_BloomTexture, clamp(v_TexCoord - off, 0.0, 1.0)).b;
            }
            else
            {
                bloom = texture(u_BloomTexture, v_TexCoord).rgb;
            }
            bloom *= u_BloomIntensity;

            // Light shafts: additive HDR streaks toward the sun, BEFORE the tonemap so a strong shaft
            // rolls off through the same operator everything else does instead of clipping. The
            // intensity carries the screen-edge fade, so a sun leaving the view takes its streaks with
            // it; when the effect is off the intensity is exactly zero and the (stale) texture is inert.
            vec3 shafts = texture(u_LightShaftTexture, v_TexCoord).rgb *
                          (u_LightShaftTintIntensity.rgb * u_LightShaftTintIntensity.a);

            // Lens flare: added in HDR alongside bloom and the shafts, before the tonemap, so a bright
            // ghost rolls off through the same operator instead of clipping to a flat disc. The intensity
            // carries the sun's screen-edge fade and is exactly zero whenever the flare pass did not run,
            // which is what makes the (stale) texture inert — the bloom image's contract.
            vec3 flare = texture(u_LensFlareTexture, v_TexCoord).rgb *
                         (u_LensFlareTintIntensity.rgb * u_LensFlareTintIntensity.a);

            vec3 color = (scene + bloom + shafts + flare) * exposure;

            // The grade (white balance, colour correction, tone curve, output device) is the baked table's.
            vec3 mapped = ColorLookupTable(color);

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

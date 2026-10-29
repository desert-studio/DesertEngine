// DesertAsset {"Kind":"Shader","Guid":"5510aa7eaf55f0c3d7f42c49c51b02ee","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomDownsample"
{
    Compute
    {
        // Progressive bloom downsample (Call of Duty: Advanced Warfare / Jimenez 2014). Each dispatch reads one
        // source mip and writes the next-smaller mip with a 13-tap filter. On the very first pass (scene HDR ->
        // bloom mip 0) a per-group Karis average is used to suppress fireflies, and the bright-pass threshold is
        // folded in here (so no separate bright-pass is needed). SourceMipTexture is bound as a single-mip view of the
        // source mip, so the whole bloom mip-chain can live in one image while each mip keeps its own layout.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass and numbered by the program build (SharedSamplerBinding.hpp); a same-name
        // macro after each texture keeps the sampled form.
        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(0) texture2D SourceMipTexture;
        #define SourceMipTexture sampler2D(SourceMipTexture, LinearClampSampler)
        layout(binding = 1, rgba16f) writeonly uniform image2D DestinationMipTexture;
        // This frame's adapted luminance (AEAverage's 1x1), or white when auto-exposure is off (then unread).
        Uniform(2) texture2D AdaptedLuminanceTexture;
        #define AdaptedLuminanceTexture sampler2D(AdaptedLuminanceTexture, PointClampSampler)
        // Local exposure (UE r.Bloom.ApplyLocalExposure, PostProcessBloomSetup.usf): the bilateral grid and the
        // blurred log luminance LocalExposureRenderer built this frame, read on the first pass only so the bloom
        // carries the same per-pixel exposure the composite gives the scene term. System.Black3D / System.Black
        // with LocalExposureSlice.w = 0 when the passes did not run (then unread).
        Uniform(3) texture3D LumBilateralGrid;
        #define LumBilateralGrid sampler3D(LumBilateralGrid, LinearClampSampler)
        Uniform(4) texture2D BlurredLogLuminance;
        #define BlurredLogLuminance sampler2D(BlurredLogLuminance, LinearClampSampler)

        PushConstant PushConstants
        {
            vec2  SourceTexelSize; // 1 / size(source mip)
            int   FirstPass;    // 1 => scene -> mip0 (Karis average + threshold)
            float Threshold;    // bright-pass cutoff (first pass only), raw luminance unless AutoExposure,
                                  // x ViewFrame::PreExposure (BloomRenderer: the scene is stored pre-exposed)
            float ExposureKey;  // auto-exposure key — the composite's ExposureKey
            int   AutoExposure; // 1 => Threshold is after exposure: divide by key / adapted luminance
            float Exposure;     // manual exposure (the composite's Exposure) when AutoExposure is 0
            float InversePreExposure;     // 1 / ViewFrame::PreExposure
            vec4  LocalExposureContrast;  // the composite's four local-exposure vectors (TonemapRenderer::FillMaterial)
            vec4  LocalExposureThreshold;
            vec4  LocalExposureUV;
            vec4  LocalExposureSlice;     // w: 1 = apply, 0 = not produced this frame
        };

        #include <Common/LocalExposure.glslh>

        LocalSize(16, 16, 1);

        float Luminance( vec3 c )
        {
            return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
        }

        // @p c scaled by its local exposure at @p tapUV (unchanged when @p apply is false).
        vec3 LocallyExposed( vec3 c, vec2 tapUV, bool apply, float logExposure )
        {
            if ( !apply )
                return c;
            float logLum  = log2( max( LocalExposureLuminance( c ) * InversePreExposure, 1.0e-10 ) );
            vec2  cell    = textureLod( LumBilateralGrid, vec3( tapUV * LocalExposureUV.xy,
                                        LocalExposureGridW( logLum, LocalExposureSlice.y, LocalExposureSlice.z ) ), 0.0 ).xy;
            float blurred = textureLod( BlurredLogLuminance, tapUV * LocalExposureUV.zw, 0.0 ).r;
            float base    = LocalExposureBaseLogLuminance( cell, blurred, LocalExposureContrast.w, logExposure );
            return c * exp2( LocalExposureLogExposure( logLum + logExposure, base, LocalExposureSlice.x,
                                                       LocalExposureContrast, LocalExposureThreshold ) );
        }

        void main()
        {
            ivec2 dstSize  = imageSize( DestinationMipTexture );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2  uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            // SourceMipTexture is a view of the ONE source mip the graph node declared (in the layout its access
            // put it in), so its only level is lod 0.
            vec2  t  = SourceTexelSize;

            // Local exposure of the first pass, per tap (the taps straddle the awning edges a single centre lookup
            // would halo across). Same law and inputs as SceneComposite's scene term.
            const bool  applyLE     = FirstPass == 1 && LocalExposureSlice.w > 0.5;
            float       logExposure = 0.0;
            if ( applyLE )
            {
                float exposure = Exposure;
                if ( AutoExposure == 1 )
                    exposure = ExposureKey / max( textureLod( AdaptedLuminanceTexture, vec2( 0.5 ), 0.0 ).r, 1e-4 );
                logExposure = log2( max( exposure, 1.0e-10 ) );
            }

            // The pass binds SourceMipTexture with a clamp-to-edge sampler (SamplerDesc::LinearClamp), so taps that
            // fall outside [0,1] read the edge texel and a bright object never wraps to the opposite edge.
        #define TAP( off ) LocallyExposed( textureLod( SourceMipTexture, uv + t * ( off ), 0.0 ).rgb, uv + t * ( off ), \
                                           applyLE, logExposure )

            // 13 taps around uv (in source texels).
            vec3 a = TAP( vec2( -2, -2 ) );
            vec3 b = TAP( vec2(  0, -2 ) );
            vec3 c = TAP( vec2(  2, -2 ) );
            vec3 d = TAP( vec2( -2,  0 ) );
            vec3 e = TAP( vec2(  0,  0 ) );
            vec3 f = TAP( vec2(  2,  0 ) );
            vec3 g = TAP( vec2( -2,  2 ) );
            vec3 h = TAP( vec2(  0,  2 ) );
            vec3 i = TAP( vec2(  2,  2 ) );
            vec3 j = TAP( vec2( -1, -1 ) );
            vec3 k = TAP( vec2(  1, -1 ) );
            vec3 m = TAP( vec2( -1,  1 ) );
            vec3 n = TAP( vec2(  1,  1 ) );
        #undef TAP

            vec3 color;
            if ( FirstPass == 1 )
            {
                // Five 4-tap boxes, each weighted by a Karis average (1 / (1 + luma)) to kill fireflies before
                // they smear across the whole bloom.
                vec3  b0 = ( a + b + d + e ) * 0.25;
                vec3  b1 = ( b + c + e + f ) * 0.25;
                vec3  b2 = ( d + e + g + h ) * 0.25;
                vec3  b3 = ( e + f + h + i ) * 0.25;
                vec3  b4 = ( j + k + m + n ) * 0.25;
                float w0 = 1.0 / ( 1.0 + Luminance( b0 ) );
                float w1 = 1.0 / ( 1.0 + Luminance( b1 ) );
                float w2 = 1.0 / ( 1.0 + Luminance( b2 ) );
                float w3 = 1.0 / ( 1.0 + Luminance( b3 ) );
                float w4 = 1.0 / ( 1.0 + Luminance( b4 ) );
                color    = ( b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4 ) / ( w0 + w1 + w2 + w3 + w4 );

                // Bright-pass: keep only the energy above the threshold (preserves hue).
                // With auto-exposure the authored threshold is in EXPOSED units, as the manual path states it
                // (SceneRenderer divides it by the manual scale on the CPU): the exposure lives on the GPU here,
                // so it is divided here, by the composite's own key / adapted luminance. Without this a
                // physical scene (a 100 000 lux sun, ~10^4 cd/m^2 everywhere) was bloom from edge to edge.
                float threshold = Threshold;
                if ( AutoExposure == 1 )
                    threshold *= max( textureLod( AdaptedLuminanceTexture, vec2( 0.5 ), 0.0 ).r, 1e-4 ) /
                                 max( ExposureKey, 1e-4 );
                float brightness   = max( color.r, max( color.g, color.b ) );
                float contribution = max( brightness - threshold, 0.0 ) / max( brightness, 1e-4 );
                color *= contribution;
            }
            else
            {
                color  = e * 0.125;
                color += ( a + c + g + i ) * 0.03125;
                color += ( b + d + f + h ) * 0.0625;
                color += ( j + k + m + n ) * 0.125;
            }

            imageStore( DestinationMipTexture, dstCoord, vec4( color, 1.0 ) );
        }
    }
}

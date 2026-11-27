// DesertAsset {"Kind":"Shader","Guid":"b71bf33bb06343d7968ad05114fc80e6","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomSetup"
{
    Compute
    {
        // Ported from UE 5.8 PostProcessBloomSetup.usf (BloomSetupCS) and PostProcessDownsample.usf (the High
        // quality filter), adapted: one dispatch writes the half-resolution bloom input straight from the scene
        // colour - four bilinear taps at +-1 source texel (a 4x4 box), each made finite and locally exposed, then
        // UE's threshold law on the exposed luminance. Both bloom methods start from this image (BloomRenderer).
        // The output stays in the scene colour's stored (pre-exposed) scale: the composite undoes it once.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass and numbered by the program build (SharedSamplerBinding.hpp); a same-name
        // macro after each texture keeps the sampled form.
        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(0) texture2D SceneColorTexture;
        #define SceneColorTexture sampler2D(SceneColorTexture, LinearClampSampler)
        layout(binding = 1, rgba16f) writeonly uniform image2D BloomSetupTexture;
        // This frame's adapted luminance (AEAverage's 1x1), or white when auto-exposure is off (then unread).
        Uniform(2) texture2D AdaptedLuminanceTexture;
        #define AdaptedLuminanceTexture sampler2D(AdaptedLuminanceTexture, PointClampSampler)
        // Local exposure (UE r.Bloom.ApplyLocalExposure): the bilateral grid and the blurred log luminance
        // LocalExposureRenderer built this frame, so the bloom carries the per-pixel exposure the composite gives
        // the scene term. System.Black3D / System.Black with LocalExposureSlice.w = 0 when the passes did not run.
        Uniform(3) texture3D LumBilateralGrid;
        #define LumBilateralGrid sampler3D(LumBilateralGrid, LinearClampSampler)
        Uniform(4) texture2D BlurredLogLuminance;
        #define BlurredLogLuminance sampler2D(BlurredLogLuminance, LinearClampSampler)

        PushConstant PushConstants
        {
            vec2  SourceTexelSize;    // 1 / size(scene colour)
            float Threshold;          // UE BloomThreshold: exposed luminance; <= -1 keeps every pixel
            float ExposureKey;        // auto-exposure key - the composite's ExposureKey
            int   AutoExposure;       // 1 => exposure = key / adapted luminance, else Exposure
            float Exposure;           // manual exposure (the composite's Exposure)
            float InversePreExposure; // 1 / ViewFrame::PreExposure
            vec4  LocalExposureContrast;  // the composite's four local-exposure vectors (TonemapRenderer::FillMaterial)
            vec4  LocalExposureThreshold;
            vec4  LocalExposureUV;
            vec4  LocalExposureSlice;     // w: 1 = apply, 0 = not produced this frame
        };

        #include <Common/LocalExposure.glslh>

        LocalSize(16, 16, 1);

        // UE Luminance (Common.ush): Rec.709 weights.
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

        // UE PostProcessBloomSetup: the scene colour the bloom starts from is clamped ("to avoid artifacts from
        // exceeding fp16"), and a non-finite texel is no energy at all: one NaN/Inf scene texel otherwise became a
        // bloomed ball over every stage (FPS-FIREFLY2). No change to a finite rgba16f scene.
        const float kBloomMaxSource = 65504.0;
        vec3 BloomFiniteSource( const vec3 c )
        {
            if ( any( isnan( c ) ) || any( isinf( c ) ) )
                return vec3( 0.0 );
            return clamp( c, vec3( 0.0 ), vec3( kBloomMaxSource ) );
        }

        void main()
        {
            ivec2 dstSize  = imageSize( BloomSetupTexture );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2 uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            vec2 t  = SourceTexelSize;

            // The composite's exposure (SceneComposite.shader): manual, or key / adapted luminance.
            float exposure = Exposure;
            if ( AutoExposure == 1 )
                exposure = ExposureKey / max( textureLod( AdaptedLuminanceTexture, vec2( 0.5 ), 0.0 ).g, 1e-4 );
            const bool  applyLE     = LocalExposureSlice.w > 0.5;
            const float logExposure = log2( max( exposure, 1.0e-10 ) );

            // UE Downsample High: four bilinear taps at the corners of the 2x2 source block, each the mean of four
            // texels (a 4x4 box). The sampler clamps to the edge, so a bright object never wraps.
        #define TAP( off ) LocallyExposed( BloomFiniteSource( textureLod( SceneColorTexture, uv + t * ( off ), 0.0 ).rgb ), \
                                           uv + t * ( off ), applyLE, logExposure )
            vec3 color = ( TAP( vec2( -1.0, -1.0 ) ) + TAP( vec2( 1.0, -1.0 ) ) + TAP( vec2( -1.0, 1.0 ) ) +
                           TAP( vec2( 1.0, 1.0 ) ) ) * 0.25;
        #undef TAP
            color = max( color, vec3( 0.0 ) );

            // UE BloomSetupCS: above a threshold of -1 every pixel blooms; otherwise its share grows by half per unit
            // of exposed luminance past the threshold (saturate((L * exposure - T) * 0.5)), hue kept.
            if ( Threshold > -1.0 )
            {
                float exposedLuminance = Luminance( color ) * InversePreExposure * exposure;
                color *= clamp( ( exposedLuminance - Threshold ) * 0.5, 0.0, 1.0 );
            }

            imageStore( BloomSetupTexture, dstCoord, vec4( color, 1.0 ) );
        }
    }
}

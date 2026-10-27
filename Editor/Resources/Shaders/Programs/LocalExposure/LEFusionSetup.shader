// DesertAsset {"Kind":"Shader","Guid":"8a9596107b4b4cc597fc88a05acf65a7","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LEFusionSetup"
{
    Compute
    {
        // LOCAL EXPOSURE FUSION SETUP — UE 5.4 FusionSetupCS (PostProcessLocalExposure.usf). Per scene pixel: the
        // exposed luminance, its highlight (x HighlightScale = 2^-EV) and shadow (x ShadowScale = 2^+EV) exposures
        // through the fusion curve (Common/LocalExposure.glslh), and their normalised well-exposedness weights.
        // The exposure is the composite's own (SceneComposite: auto = key / adapted luminance, else manual), read
        // from the same 1x1 adapted luminance in the same frame, so the setup and the apply agree on it.

        LocalSize(8, 8, 1);

        Uniform(0) texture2D SceneColorTexture;
        Uniform(1) texture2D AverageLuminanceTexture;
        Uniform sampler PointClampSampler;
        #define SceneColorTexture sampler2D(SceneColorTexture, PointClampSampler)
        #define AverageLuminanceTexture sampler2D(AverageLuminanceTexture, PointClampSampler)
        layout(binding = 2, rgba16f) writeonly uniform image2D OutExposures;
        layout(binding = 3, rgba16f) writeonly uniform image2D OutWeights;

        PushConstant PushConstants
        {
            float InversePreExposure; // the scene colour is stored pre-exposed
            float ManualExposure;     // the composite's Exposure (used when AutoExposure < 0.5)
            float ExposureKey;        // the composite's ExposureKey
            float AutoExposure;       // > 0.5: exposure = key / adapted luminance
            float HighlightScale;     // 2^-lerp(6, 0, highlight contrast)
            float ShadowScale;        // 2^+lerp(6, 0, shadow contrast)
            float TargetLuminance;    // UE r.LocalExposure.ExposureFusion.TargetLuminance
            float Reinhard;           // > 0.5: the scene operator is Reinhard
        };

        #include <Common/LocalExposure.glslh>

        void main()
        {
            const ivec2 size  = textureSize( SceneColorTexture, 0 );
            const ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x >= size.x || coord.y >= size.y )
                return;
            float exposure = ManualExposure;
            if ( AutoExposure > 0.5 )
                exposure = ExposureKey / max( texelFetch( AverageLuminanceTexture, ivec2( 0 ), 0 ).r, 1.0e-4 );

            const bool  reinhard = Reinhard > 0.5;
            const vec3  scene    = texelFetch( SceneColorTexture, coord, 0 ).rgb;
            const float lum      = max( LocalExposureLuminance( scene ), 0.0 ) * InversePreExposure * exposure;

            const vec3 tonemapped = vec3( LocalExposureFusionTonemap( lum, reinhard ),
                                          LocalExposureFusionTonemap( lum * HighlightScale, reinhard ),
                                          LocalExposureFusionTonemap( lum * ShadowScale, reinhard ) );
            vec3 weights = vec3( LocalExposureFusionWeight( tonemapped.x, TargetLuminance ),
                                 LocalExposureFusionWeight( tonemapped.y, TargetLuminance ),
                                 LocalExposureFusionWeight( tonemapped.z, TargetLuminance ) );
            weights /= max( dot( weights, vec3( 1.0 ) ), 1.0e-6 );

            imageStore( OutExposures, coord, vec4( tonemapped, 0.0 ) );
            imageStore( OutWeights, coord, vec4( weights, 0.0 ) );
        }
    }
}

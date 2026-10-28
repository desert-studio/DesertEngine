// DesertAsset {"Kind":"Shader","Guid":"2e9a6c28739847a0b4b8a30e66785114","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LEFusionBlend"
{
    Compute
    {
        // LOCAL EXPOSURE FUSION BLEND — UE 5.4 FusionBlendCS, coarsest level first. At the coarsest level the
        // result is the weighted mean of the three exposures; every finer level adds its weighted Laplacian band
        // (level exposures minus the upsampled coarser level) to the upsampled coarser result. Level 0 is the
        // fused display value the composite reads (LocalExposureFusionExposure).

        LocalSize(8, 8, 1);

        Uniform(0) texture2D InputExposures;
        Uniform(1) texture2D InputWeights;
        Uniform(2) texture2D CoarserExposures;
        Uniform(3) texture2D PrevResult;
        Uniform sampler LinearClampSampler;
        #define InputExposures sampler2D(InputExposures, LinearClampSampler)
        #define InputWeights sampler2D(InputWeights, LinearClampSampler)
        #define CoarserExposures sampler2D(CoarserExposures, LinearClampSampler)
        #define PrevResult sampler2D(PrevResult, LinearClampSampler)
        layout(binding = 4, r32f) writeonly uniform image2D OutResult;

        PushConstant PushConstants
        {
            vec2  OutputSize;     // this level's size
            vec2  InvCoarserSize; // 1 / the coarser level's size
            float Laplacian;      // > 0.5: a coarser level exists (UE LAPLACIAN permutation)
            float Pad0;
            float Pad1;
            float Pad2;
        };

        void main()
        {
            const ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x >= int( OutputSize.x ) || coord.y >= int( OutputSize.y ) )
                return;
            vec3 weights = texelFetch( InputWeights, coord, 0 ).rgb;
            weights /= max( dot( weights, vec3( 1.0 ) ), 1.0e-6 );
            vec3  lums = texelFetch( InputExposures, coord, 0 ).rgb;
            float prev = 0.0;
            if ( Laplacian > 0.5 )
            {
                // Pixel coordinate p here is p / 2 on the coarser level (its texel j sits over our 2j + 1).
                const vec2 coarserUV = ( vec2( coord ) + 0.5 ) * 0.5 * InvCoarserSize;
                lums -= textureLod( CoarserExposures, coarserUV, 0.0 ).rgb;
                prev = textureLod( PrevResult, coarserUV, 0.0 ).r;
            }
            imageStore( OutResult, coord, vec4( prev + dot( lums, weights ), 0.0, 0.0, 0.0 ) );
        }
    }
}

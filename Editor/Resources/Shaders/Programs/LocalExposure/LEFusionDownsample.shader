// DesertAsset {"Kind":"Shader","Guid":"a21b20292fdf43af812ec89cf6f3fed7","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LEFusionDownsample"
{
    Compute
    {
        // LOCAL EXPOSURE FUSION DOWNSAMPLE — one level of the two Gaussian pyramids (exposures and weights) the
        // fusion blends through (UE FTextureDownsampleChain, EDownsampleQuality::High). Level i texel j is centred
        // on level i-1 pixel coordinate 2j+1; five bilinear taps (centre 1/2, four diagonals at +-1 texel 1/8 each)
        // = a [1 3 3 1]-like tent over 4x4 source texels. Clamp addressing at the borders.

        LocalSize(8, 8, 1);

        Uniform(0) texture2D InputExposures;
        Uniform(1) texture2D InputWeights;
        Uniform sampler LinearClampSampler;
        #define InputExposures sampler2D(InputExposures, LinearClampSampler)
        #define InputWeights sampler2D(InputWeights, LinearClampSampler)
        layout(binding = 2, rgba16f) writeonly uniform image2D OutExposures;
        layout(binding = 3, rgba16f) writeonly uniform image2D OutWeights;

        PushConstant PushConstants
        {
            vec2 InvInputSize; // 1 / level i-1 size
            vec2 OutputSize;   // level i size
        };

        void main()
        {
            const ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x >= int( OutputSize.x ) || coord.y >= int( OutputSize.y ) )
                return;
            const vec2 uv = ( 2.0 * vec2( coord ) + 1.0 ) * InvInputSize;
            vec3 e = textureLod( InputExposures, uv, 0.0 ).rgb * 0.5;
            vec3 w = textureLod( InputWeights, uv, 0.0 ).rgb * 0.5;
            const vec2 offsets[4] = vec2[4]( vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ), vec2( 1.0, 1.0 ) );
            for ( int i = 0; i < 4; ++i )
            {
                const vec2 tap = uv + offsets[i] * InvInputSize;
                e += textureLod( InputExposures, tap, 0.0 ).rgb * 0.125;
                w += textureLod( InputWeights, tap, 0.0 ).rgb * 0.125;
            }
            imageStore( OutExposures, coord, vec4( e, 0.0 ) );
            imageStore( OutWeights, coord, vec4( w, 0.0 ) );
        }
    }
}

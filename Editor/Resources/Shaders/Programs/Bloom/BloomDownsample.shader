// DesertAsset {"Kind":"Shader","Guid":"5510aa7eaf55f0c3d7f42c49c51b02ee","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomDownsample"
{
    Compute
    {
        // Ported from UE 5.8 PostProcessDownsample.usf (High quality), adapted: compute, one dispatch per stage of
        // the bloom chain (BloomRenderer: the setup's 1/2 down to 1/64). Four bilinear taps at the corners of the
        // destination texel's 2x2 source block - a 4x4 box - and nothing else: no threshold, no Karis average.
        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D SourceTexture;
        #define SourceTexture sampler2D(SourceTexture, LinearClampSampler)
        layout(binding = 1, rgba16f) writeonly uniform image2D DestinationTexture;

        PushConstant PushConstants
        {
            vec2 SourceTexelSize; // 1 / size(source stage)
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 dstSize  = imageSize( DestinationTexture );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2 uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            vec2 t  = SourceTexelSize;
            // The sampler clamps to the edge, so a bright object never wraps to the opposite side.
            vec3 color = textureLod( SourceTexture, uv + t * vec2( -1.0, -1.0 ), 0.0 ).rgb +
                         textureLod( SourceTexture, uv + t * vec2( 1.0, -1.0 ), 0.0 ).rgb +
                         textureLod( SourceTexture, uv + t * vec2( -1.0, 1.0 ), 0.0 ).rgb +
                         textureLod( SourceTexture, uv + t * vec2( 1.0, 1.0 ), 0.0 ).rgb;
            imageStore( DestinationTexture, dstCoord, vec4( max( color * 0.25, vec3( 0.0 ) ), 1.0 ) );
        }
    }
}

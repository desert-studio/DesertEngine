// DesertAsset {"Kind":"Shader","Guid":"2c2d1ef42e4d8ba0ae40a7a05a73aa74","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomUpsample"
{
    Compute
    {
        // Progressive bloom upsample (COD / Jimenez). Each dispatch samples a smaller mip with a 3x3 tent filter
        // and ADDS it into the next-larger mip (read-modify-write on the storage image), walking the chain back
        // up to mip 0. DestinationMipTexture is the larger (destination) mip; SourceMipTexture is a single-mip view of the smaller mip.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(2) sampler LinearClampSampler;
        Uniform(0) texture2D SourceMipTexture;
        #define SourceMipTexture sampler2D(SourceMipTexture, LinearClampSampler)
        layout(binding = 1, rgba16f) uniform image2D DestinationMipTexture;

        PushConstant PushConstants
        {
            vec2  SourceTexelSize; // 1 / size(source / smaller mip)
            float FilterRadius; // tent radius in source texels
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 dstSize  = imageSize( DestinationMipTexture );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2  uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            // SourceMipTexture is a view of the ONE source mip the graph node declared (in the layout its access
            // put it in), so its only level is lod 0.
            vec2  o  = SourceTexelSize * FilterRadius;

            // The pass binds SourceMipTexture with a clamp-to-edge sampler (SamplerDesc::LinearClamp), so tent taps
            // outside [0,1] read the edge texel and a bright object's glow never wraps to the opposite edge.
        #define TAP( coord ) textureLod( SourceMipTexture, ( coord ), 0.0 ).rgb

            // 3x3 tent (weights 1 2 1 / 2 4 2 / 1 2 1).
            vec3 a = TAP( uv + vec2( -o.x,  o.y ) );
            vec3 b = TAP( uv + vec2(  0.0,  o.y ) );
            vec3 c = TAP( uv + vec2(  o.x,  o.y ) );
            vec3 d = TAP( uv + vec2( -o.x,  0.0 ) );
            vec3 e = TAP( uv );
            vec3 f = TAP( uv + vec2(  o.x,  0.0 ) );
            vec3 g = TAP( uv + vec2( -o.x, -o.y ) );
            vec3 h = TAP( uv + vec2(  0.0, -o.y ) );
            vec3 i = TAP( uv + vec2(  o.x, -o.y ) );
        #undef TAP

            vec3 up = ( e * 4.0 + ( b + d + f + h ) * 2.0 + ( a + c + g + i ) ) * ( 1.0 / 16.0 );

            vec3 dst = imageLoad( DestinationMipTexture, dstCoord ).rgb;
            imageStore( DestinationMipTexture, dstCoord, vec4( dst + up, 1.0 ) );
        }
    }
}

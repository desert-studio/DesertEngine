Shader "BloomUpsample"
{
    Compute
    {
        // Progressive bloom upsample (COD / Jimenez). Each dispatch samples a smaller mip with a 3x3 tent filter
        // and ADDS it into the next-larger mip (read-modify-write on the storage image), walking the chain back
        // up to mip 0. u_Output is the larger (destination) mip; u_Source is sampled at the smaller mip's LOD.

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) uniform image2D u_Output;

        PushConstant PushConstants
        {
            vec2  u_SrcTexelSize; // 1 / size(source / smaller mip)
            int   u_SrcMip;       // LOD to sample from u_Source
            float u_FilterRadius; // tent radius in source texels
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 dstSize  = imageSize( u_Output );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2  uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            float l  = float( u_SrcMip );
            vec2  o  = u_SrcTexelSize * u_FilterRadius;

            // Clamp to the source mip's valid texel-centre range: the global sampler is REPEAT, so tent taps
            // that fall outside [0,1] would wrap a bright object's glow to the opposite screen edge.
            vec2 lo = 0.5 * u_SrcTexelSize;
            vec2 hi = 1.0 - 0.5 * u_SrcTexelSize;
        #define TAP( coord ) textureLod( u_Source, clamp( ( coord ), lo, hi ), l ).rgb

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

            vec3 dst = imageLoad( u_Output, dstCoord ).rgb;
            imageStore( u_Output, dstCoord, vec4( dst + up, 1.0 ) );
        }
    }
}

Shader "BloomDownsample"
{
    Compute
    {
        // Progressive bloom downsample (Call of Duty: Advanced Warfare / Jimenez 2014). Each dispatch reads one
        // source mip and writes the next-smaller mip with a 13-tap filter. On the very first pass (scene HDR ->
        // bloom mip 0) a per-group Karis average is used to suppress fireflies, and the bright-pass threshold is
        // folded in here (so no separate bright-pass is needed). u_Source is sampled with an explicit LOD so the
        // whole bloom mip-chain can live in one image.

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) writeonly uniform image2D u_Output;

        PushConstant PushConstants
        {
            vec2  u_SrcTexelSize; // 1 / size(source mip)
            int   u_SrcMip;       // LOD to sample from u_Source
            int   u_FirstPass;    // 1 => scene -> mip0 (Karis average + threshold)
            float u_Threshold;    // bright-pass cutoff (first pass only)
        };

        LocalSize(16, 16, 1);

        float Luminance( vec3 c )
        {
            return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
        }

        void main()
        {
            ivec2 dstSize  = imageSize( u_Output );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2  uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            float l  = float( u_SrcMip );
            vec2  t  = u_SrcTexelSize;

            // The global sampler addresses in REPEAT mode, so taps that fall outside [0,1] would wrap to the
            // opposite edge and smear a bright object across the whole screen (worse at small mips where t is
            // large). Clamp each sample to the source's valid texel-centre range so bloom never wraps.
            vec2 lo = 0.5 * t;
            vec2 hi = 1.0 - 0.5 * t;
        #define TAP( off ) textureLod( u_Source, clamp( uv + t * ( off ), lo, hi ), l ).rgb

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
            if ( u_FirstPass == 1 )
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
                float brightness   = max( color.r, max( color.g, color.b ) );
                float contribution = max( brightness - u_Threshold, 0.0 ) / max( brightness, 1e-4 );
                color *= contribution;
            }
            else
            {
                color  = e * 0.125;
                color += ( a + c + g + i ) * 0.03125;
                color += ( b + d + f + h ) * 0.0625;
                color += ( j + k + m + n ) * 0.125;
            }

            imageStore( u_Output, dstCoord, vec4( color, 1.0 ) );
        }
    }
}

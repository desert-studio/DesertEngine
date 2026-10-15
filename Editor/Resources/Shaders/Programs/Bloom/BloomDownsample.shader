// DesertAsset {"Kind":"Shader","Guid":"5510aa7eaf55f0c3d7f42c49c51b02ee","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomDownsample"
{
    Compute
    {
        // Progressive bloom downsample (Call of Duty: Advanced Warfare / Jimenez 2014). Each dispatch reads one
        // source mip and writes the next-smaller mip with a 13-tap filter. On the very first pass (scene HDR ->
        // bloom mip 0) every tap goes through UE's bloom setup threshold first (PostProcessBloom.usf:37-66, on the
        // EXPOSED luminance) and a per-group Karis average then suppresses fireflies (so no separate bright-pass). u_Source is bound as a single-mip view of the
        // source mip, so the whole bloom mip-chain can live in one image while each mip keeps its own layout.

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) writeonly uniform image2D u_Output;
        // This frame's adapted luminance - the image the tonemap reads its exposure from (System.White without eye
        // adaptation, when u_Exposure says manual). Read on the first pass only.
        Uniform(2) sampler2D u_AvgLuminance;

        PushConstant PushConstants
        {
            vec2  u_SrcTexelSize; // 1 / size(source mip)
            int   u_FirstPass;    // 1 => scene -> mip0 (Karis average + threshold)
            float u_Threshold;    // bloom threshold on the EXPOSED luminance (first pass only), UE's BloomThreshold
            vec4  u_Exposure;     // Graphic::ViewExposureOf - the tonemap's exposure, from the same settings
        };

        #include <Common/ViewExposure.glslh>

        LocalSize(16, 16, 1);

        float Luminance( vec3 c )
        {
            return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
        }

        // UE's BloomSetupCommon (PostProcessBloom.usf:37-66) without local exposure: the amount of a colour that
        // blooms ramps from 0 at the threshold to 1 two units of EXPOSED luminance above it, and the colour keeps its
        // scene-linear scale (the tonemap exposes bloom with the scene). With auto-exposure the exposure is the
        // adapted one, so the same picture blooms the same whether it is exposed by hand or by the eye.
        vec3 BloomSetup( vec3 linearColor, float exposureScale )
        {
            float totalLuminance = Luminance( linearColor ) * exposureScale;
            float bloomAmount    = clamp( ( totalLuminance - u_Threshold ) * 0.5, 0.0, 1.0 );
            return bloomAmount * linearColor;
        }

        void main()
        {
            ivec2 dstSize  = imageSize( u_Output );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2  uv = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            // u_Source is a view of the ONE source mip the graph node declared (in the layout its access
            // put it in), so its only level is lod 0.
            vec2  t  = u_SrcTexelSize;

            // The pass binds u_Source with a clamp-to-edge sampler (SamplerDesc::LinearClamp), so taps that
            // fall outside [0,1] read the edge texel and a bright object never wraps to the opposite edge.
        #define TAP( off ) textureLod( u_Source, uv + t * ( off ), 0.0 ).rgb

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
                // Threshold every tap on the exposed luminance (UE thresholds each scene pixel), then five 4-tap
                // boxes, each weighted by a Karis average (1 / (1 + luma)) to kill fireflies before they smear
                // across the whole bloom.
                float exposureScale = ViewExposureScale( u_Exposure, texture( u_AvgLuminance, vec2( 0.5 ) ).r );
                a = BloomSetup( a, exposureScale );
                b = BloomSetup( b, exposureScale );
                c = BloomSetup( c, exposureScale );
                d = BloomSetup( d, exposureScale );
                e = BloomSetup( e, exposureScale );
                f = BloomSetup( f, exposureScale );
                g = BloomSetup( g, exposureScale );
                h = BloomSetup( h, exposureScale );
                i = BloomSetup( i, exposureScale );
                j = BloomSetup( j, exposureScale );
                k = BloomSetup( k, exposureScale );
                m = BloomSetup( m, exposureScale );
                n = BloomSetup( n, exposureScale );

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

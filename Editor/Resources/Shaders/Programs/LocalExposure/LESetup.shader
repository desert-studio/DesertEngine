// DesertAsset {"Kind":"Shader","Guid":"ccc62f2e4523428f924713d543b8840e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LESetup"
{
    Compute
    {
        // LOCAL EXPOSURE SETUP — ONE pass for UE's bilateral-grid pass (PostProcessHistogram.cpp:484-542), its
        // half-res downsample chain to 1/32 (PostProcessing.cpp:1414-1426) and SetupLogLuminance
        // (PostProcessLocalExposure.usf:26-39) — CONTRACT.md §6 item 1.
        //
        // A group = one grid cell = 64x64 HALF-res texels = 128x128 output pixels. Each of the 8x8 threads owns a
        // contiguous 8x8 block of half-res texels; one bilinear tap of the output-res scene at a half-res texel's
        // centre is UE's half-res 2x2 box. Per texel: log2(radiance luminance) -> slice (1 EV, centred on the last
        // exposure's middle grey) -> (sum logLum, count) per slice (UE PostProcessHistogram.usf:56-58,199-225).
        // A 1/32 texel = 16x16 half-res texels = 2x2 threads: their summed linear luminance -> log2(mean) into the
        // 1/32 target. Texels outside the view are not counted.

        LocalSize(8, 8, 1);

        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D SceneColorTexture;
        #define SceneColorTexture sampler2D(SceneColorTexture, LinearClampSampler)
        layout(binding = 1, rgba32f) writeonly uniform image3D OutBilateralGrid;
        layout(binding = 2, r32f) writeonly uniform image2D OutLogLuminance;

        PushConstant PushConstants
        {
            vec2  InvOutputSize;    // 1 / the scene's size in pixels
            vec2  OutputSize;       // the scene's size in pixels
            float InversePreExposure; // the scene colour is stored pre-exposed
            float SliceLogMin;      // log2 luminance at the bottom of slice 0 (LocalExposureSliceLogMin)
            float InvSliceEV;       // 1 / stops per slice
            float Pad0;
        };

        #include <Common/LocalExposure.glslh>

        shared float s_Sum[LOCAL_EXPOSURE_GRID_DEPTH * 64];
        shared float s_Count[LOCAL_EXPOSURE_GRID_DEPTH * 64];
        shared float s_Lum[64];
        shared float s_LumCount[64];

        void main()
        {
            const uint local = gl_LocalInvocationIndex;
            for ( int s = 0; s < LOCAL_EXPOSURE_GRID_DEPTH; ++s )
            {
                s_Sum[s * 64 + int( local )]   = 0.0;
                s_Count[s * 64 + int( local )] = 0.0;
            }

            const ivec2 halfBase = ivec2( gl_WorkGroupID.xy ) * 64 + ivec2( gl_LocalInvocationID.xy ) * 8;
            float linearSum = 0.0;
            float texels    = 0.0;
            for ( int y = 0; y < 8; ++y )
            {
                for ( int x = 0; x < 8; ++x )
                {
                    const vec2 pixel = vec2( halfBase + ivec2( x, y ) ) * 2.0 + 1.0; // the 2x2 output pixels' corner
                    if ( pixel.x >= OutputSize.x || pixel.y >= OutputSize.y )
                        continue;
                    const vec2  uv  = min( pixel, OutputSize - 0.5 ) * InvOutputSize;
                    const float lum = max( LocalExposureLuminance( textureLod( SceneColorTexture, uv, 0.0 ).rgb ) *
                                                InversePreExposure,
                                           1.0e-10 );
                    const float logLum = log2( lum );
                    const int   slice  = LocalExposureSliceIndex( logLum, SliceLogMin, InvSliceEV );
                    s_Sum[slice * 64 + int( local )] += logLum;
                    s_Count[slice * 64 + int( local )] += 1.0;
                    linearSum += lum;
                    texels += 1.0;
                }
            }
            s_Lum[local]      = linearSum;
            s_LumCount[local] = texels;
            barrier();

            // One thread per slice reduces its row of 64 threads: one write per (cell, slice).
            if ( local < uint( LOCAL_EXPOSURE_GRID_DEPTH ) )
            {
                float sum   = 0.0;
                float count = 0.0;
                for ( int i = 0; i < 64; ++i )
                {
                    sum += s_Sum[int( local ) * 64 + i];
                    count += s_Count[int( local ) * 64 + i];
                }
                imageStore( OutBilateralGrid, ivec3( gl_WorkGroupID.xy, int( local ) ), vec4( sum, count, 0.0, 0.0 ) );
            }

            // The 1/32 log luminance: the thread at the even corner of each 2x2 quad writes the quad's mean.
            const uvec2 t = gl_LocalInvocationID.xy;
            if ( ( t.x & 1u ) == 0u && ( t.y & 1u ) == 0u )
            {
                const uint  i0    = t.y * 8u + t.x;
                const float lum   = s_Lum[i0] + s_Lum[i0 + 1u] + s_Lum[i0 + 8u] + s_Lum[i0 + 9u];
                const float count = s_LumCount[i0] + s_LumCount[i0 + 1u] + s_LumCount[i0 + 8u] + s_LumCount[i0 + 9u];
                const ivec2 texel = ivec2( gl_WorkGroupID.xy ) * 4 + ivec2( t >> 1u );
                if ( count > 0.0 && all( lessThan( texel, imageSize( OutLogLuminance ) ) ) )
                    imageStore( OutLogLuminance, texel, vec4( log2( max( lum / count, 1.0e-10 ) ), 0.0, 0.0, 0.0 ) );
            }
        }
    }
}

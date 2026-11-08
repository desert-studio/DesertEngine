// DesertAsset {"Kind":"Shader","Guid":"dbd0675f0a1c4af2bffe27c1cc9b2b51","Versions":{"SHDR":1},"Dependencies":[]}
// Ported from UE 5.8 Shaders/Private/StochasticLighting/StochasticLightingTileClassification.usf (the pass that
// tests every pixel's history taps once a frame and packs them, PackedPixelData, for the temporal resolve) and
// Renderer/Private/Lumen/LumenScreenProbeFiltering.cpp (r.Lumen.ScreenProbeGather.SpatialFilter.
// DisocclusionMaxFrames / DisocclusionFraction, the ScreenProbeDisocclusion ScreenProbeFilterGatherTracesCS reads).
// THE SCREEN PROBE GATHER, HISTORY CLASSIFICATION: one workgroup per uniform tile (the grid shifted by the jitter,
// the strip before it included), four pixels a thread. Every surface pixel's history taps are reprojected and
// tested here, ONCE (Common/ScreenProbeTemporal.glslh, the temporal filter's own dither), and
//   ScreenProbeHistoryTaps  the packed taps (SPPackHistoryTaps), which the temporal filter blends with instead of
//                           reprojecting again: the history rejection is the temporal resolve's, measured early;
//   disocclusion            per uniform tile, the share of its surface pixels whose history holds fewer than
//                           SP_DISOCCLUSION_MAX_FRAMES frames. The temporal filter will lean on this frame there, so
//                           the spatial filter relaxes its angle test for every probe of the tile (more neighbours,
//                           less noise where the history cannot help). A tile is measured once, not once per
//                           adaptive probe in it.
// No history (first frame, a cut, a resize): no tap is kept, every tile is disoccluded.
Shader "ScreenProbeDisocclusion"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(4) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(5) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Uniform(6) texture2D Velocity;
        #define Velocity sampler2D(Velocity, PointClampSampler)
        Uniform(7) texture2D DiffuseIndirectHistory;
        Uniform(8) texture2D HistoryGuide;
        WriteBuffer(9) ScreenProbeDisocclusionUB
        {
            float screenProbeDisocclusion[]; // per uniform tile (SPUniformTileIndex): the disoccluded share, [0, 1]
        };
        Uniform(10) utexture2D GBufferShadingWord; // the foliage flag (DesertHasBackfaceDiffuse)
        #define GBufferShadingWord usampler2D(GBufferShadingWord, PointClampSampler)
        layout(binding = 11, rg32ui) restrict writeonly uniform uimage2D ScreenProbeHistoryTaps;
        #include <Generated/ShadingModels.glslh>
        #define SCREEN_PROBE_HISTORY_DECLARED
        #define SCREEN_PROBE_HISTORY_REPROJECT
        #include <Common/ScreenProbeTemporal.glslh>

        shared uint s_Surface;
        shared uint s_Disoccluded;

        void main()
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                s_Surface     = 0u;
                s_Disoccluded = 0u;
            }
            barrier();

            // Group g covers uniform tile g - 1 (group 0: the strip before the jittered grid); a thread reads the
            // same pixel of each of the tile's (SP_DOWNSAMPLE / 8)² 8x8 blocks, so a block's 64 reads are coalesced.
            // (ScreenProbes.glslh SPJitter / SPUniformTileInside / SPUniformTileIndex, spelled here: this pass binds
            // no probe buffer, which that section of the header needs.)
            const uvec2 jitter     = uvec2( screenProbeParams.atlas.w & 0xFFFFu, screenProbeParams.atlas.w >> 16u );
            const ivec2 tile       = ivec2( gl_WorkGroupID.xy ) - 1;
            const ivec2 tileOrigin = tile * int( SP_DOWNSAMPLE ) + ivec2( jitter );
            const uint  frame      = screenProbeParams.atlas.y;
            uint        surface    = 0u;
            uint        disoccluded = 0u;
            const int   side        = int( SP_DOWNSAMPLE / 8u );
            for ( int k = 0; k < side * side; ++k )
            {
                const ivec2 pixel = tileOrigin + ivec2( gl_LocalInvocationID.xy ) + ivec2( k % side, k / side ) * 8;
                if ( any( lessThan( pixel, ivec2( 0 ) ) ) ||
                     any( greaterThanEqual( pixel, ivec2( giFrame.screen.xy ) ) ) )
                    continue;
                const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
                if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
                    continue; // the sky: the temporal filter reads no taps there
                const vec2  uv    = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
                const float depth = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
                const vec3  p     = ReconstructWorldPosition( uv, depth, giFrame.invViewProjection );
                vec2        fraction;
                const SPHistoryTaps taps = SPHistoryReproject( pixel, depth, p, normalize( gb.xyz ),
                                                               SPPixelIsFoliage( pixel ),
                                                               SPTemporalNoise( uvec2( pixel ), frame ), fraction );
                imageStore( ScreenProbeHistoryTaps, pixel, uvec4( SPPackHistoryTaps( taps, fraction ), 0u, 0u ) );
                ++surface;
                if ( SPHistoryFrames( taps ) < SP_DISOCCLUSION_MAX_FRAMES )
                    ++disoccluded;
            }
            if ( any( lessThan( tile, ivec2( 0 ) ) ) ||
                 any( greaterThanEqual( uvec2( tile ), screenProbeParams.grid.xy ) ) )
                return; // the strip outside the grid: taps only, no probe reads its share
            atomicAdd( s_Surface, surface );
            atomicAdd( s_Disoccluded, disoccluded );
            barrier();
            if ( gl_LocalInvocationIndex == 0u )
                screenProbeDisocclusion[uint( tile.y ) * screenProbeParams.grid.x + uint( tile.x )] =
                     s_Surface > 0u ? float( s_Disoccluded ) / float( s_Surface ) : 1.0;
        }
    }
}

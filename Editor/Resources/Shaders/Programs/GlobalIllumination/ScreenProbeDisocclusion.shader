// DesertAsset {"Kind":"Shader","Guid":"dbd0675f0a1c4af2bffe27c1cc9b2b51","Versions":{"SHDR":1},"Dependencies":[]}
// Ported from UE 5.8 Renderer/Private/Lumen/LumenScreenProbeFiltering.cpp
// (r.Lumen.ScreenProbeGather.SpatialFilter.DisocclusionMaxFrames / DisocclusionFraction, the ScreenProbeDisocclusion
// texture ScreenProbeFilterGatherTracesCS reads), adapted: measured here, one workgroup per probe, from the history
// taps of Common/ScreenProbeTemporal.glslh (the ones the temporal filter will keep).
// THE SCREEN PROBE GATHER, DISOCCLUSION: the share of the pixels of a probe's uniform tile whose history holds
// fewer than SP_DISOCCLUSION_MAX_FRAMES frames. The temporal filter will lean on this frame there, so the spatial
// filter relaxes its angle test for such a probe (more neighbours, less noise where the history cannot help).
// No history (first frame, a cut, a resize): every probe is disoccluded.
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
        ReadBuffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        ReadBuffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler u_PointClampSampler;
        Uniform(4) texture2D u_GBufferDepth;
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        Uniform(5) texture2D u_GBufferB;
        #define u_GBufferB sampler2D(u_GBufferB, u_PointClampSampler)
        Uniform(6) texture2D Velocity;
        #define Velocity sampler2D(Velocity, u_PointClampSampler)
        Uniform(7) texture2D DiffuseIndirectHistory;
        #define DiffuseIndirectHistory sampler2D(DiffuseIndirectHistory, u_PointClampSampler)
        Uniform(8) texture2D HistoryGuide;
        #define HistoryGuide sampler2D(HistoryGuide, u_PointClampSampler)
        WriteBuffer(9) ScreenProbeDisocclusionUB
        {
            float screenProbeDisocclusion[]; // per probe: the disoccluded share of its tile, [0, 1]
        };
        Uniform(10) utexture2D GBufferShadingWord; // the foliage flag (DesertHasBackfaceDiffuse)
        #define GBufferShadingWord usampler2D(GBufferShadingWord, u_PointClampSampler)
        #include <Generated/ShadingModels.glslh>
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define SCREEN_PROBE_HISTORY_DECLARED
        #include <Common/ScreenProbeTemporal.glslh>

        shared uint s_Surface;
        shared uint s_Disoccluded;

        void main()
        {
            const uint probe         = gl_WorkGroupID.x;
            const uint uniformProbes = screenProbeParams.grid.z;
            if ( probe >= uniformProbes &&
                 probe - uniformProbes >= min( screenProbeIndex[0], screenProbeParams.grid.w ) )
                return;
            const ScreenProbe state = screenProbes[probe];
            if ( state.position.w <= 0.0 )
                return;
            if ( ( screenProbeParams.atlas.z & SP_FLAG_HISTORY_VALID ) == 0u )
            {
                if ( gl_LocalInvocationIndex == 0u )
                    screenProbeDisocclusion[probe] = 1.0;
                return;
            }
            if ( gl_LocalInvocationIndex == 0u )
            {
                s_Surface     = 0u;
                s_Disoccluded = 0u;
            }
            barrier();

            // The tile's SP_DOWNSAMPLE² pixels, a 2x2 block per thread (the grid is shifted by the jitter).
            const ivec2 tileOrigin = SPUniformTileOf( SPProbePixelOf( state ) ) * int( SP_DOWNSAMPLE ) + ivec2( SPJitter() );
            const ivec2 block      = tileOrigin + ivec2( gl_LocalInvocationID.xy ) * 2;
            uint        surface    = 0u;
            uint        disoccluded = 0u;
            for ( int k = 0; k < 4; ++k )
            {
                const ivec2 pixel = block + ivec2( k & 1, k >> 1 );
                if ( any( lessThan( pixel, ivec2( 0 ) ) ) || any( greaterThanEqual( pixel, ivec2( giFrame.screen.xy ) ) ) )
                    continue;
                const vec4 gb = texelFetch( u_GBufferB, pixel, 0 );
                if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
                    continue;
                const vec2  uv    = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
                const float depth = texelFetch( u_GBufferDepth, pixel, 0 ).r;
                const vec3  p     = ReconstructWorldPosition( uv, depth, giFrame.invViewProjection );
                // The threshold's dither at its middle: the share, not one frame's coin flips, decides.
                const SPHistoryTaps taps =
                     SPHistoryReproject( pixel, depth, p, normalize( gb.xyz ), SPPixelIsFoliage( pixel ), 0.5 );
                ++surface;
                if ( SPHistoryFrames( taps ) < SP_DISOCCLUSION_MAX_FRAMES )
                    ++disoccluded;
            }
            atomicAdd( s_Surface, surface );
            atomicAdd( s_Disoccluded, disoccluded );
            barrier();
            if ( gl_LocalInvocationIndex == 0u )
                screenProbeDisocclusion[probe] = s_Surface > 0u ? float( s_Disoccluded ) / float( s_Surface ) : 1.0;
        }
    }
}

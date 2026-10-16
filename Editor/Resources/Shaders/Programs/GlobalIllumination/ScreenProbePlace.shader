// DesertAsset {"Kind":"Shader","Guid":"940b3760fc734b088d9128944ada0771","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 1/4 — PLACE (UE ScreenProbeDownsampleDepthUniformCS + ScreenProbeAdaptivePlacementCS):
// one thread per position of the level in the push constant, every grid shifted by the frame's jitter. Level 0
// puts a uniform probe in every 16-pixel tile (on the jittered pixel when it holds geometry), resets the adaptive
// count and its tile's adaptive counts. Levels 1 and 2 skip the positions a coarser grid already holds (UE
// `any((DispatchThreadId.xy & 0x1) != 0)`) and put an adaptive probe on a position with geometry wherever the
// probes already placed cannot interpolate it (SPInterpolate under SP_ADAPTIVE_EPSILON, the integrate's own test),
// while the adaptive budget lasts, listed under its uniform tile.
Shader "ScreenProbePlace"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/ScreenProbes.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        Buffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        Buffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        Uniform(4) sampler2D u_GBufferDepth;
        Uniform(5) sampler2D u_GBufferB;

        PushConstant ScreenProbePlacePush
        {
            uint u_Level;
        };

        void main()
        {
            const vec2  extent = giFrame.screen.xy;
            const uint  level  = u_Level;
            const uvec2 grid   = SPGridSize( level, extent );
            const ivec2 cell   = ivec2( gl_GlobalInvocationID.xy );
            if ( level == 0u && cell == ivec2( 0 ) )
                screenProbeIndex[0] = 0u;
            if ( any( greaterThanEqual( uvec2( cell ), grid ) ) )
                return;
            if ( level > 0u && all( equal( cell & 1, ivec2( 0 ) ) ) )
                return; // a coarser level's position

            ivec2 pixel = SPProbePixel( level, cell, SPJitter() );
            if ( level == 0u )
                pixel = min( pixel, ivec2( extent ) - 1 );
            else if ( any( greaterThanEqual( pixel, ivec2( extent ) ) ) )
                return;
            const vec2 uv     = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3 normal = texelFetch( u_GBufferB, pixel, 0 ).xyz;
            ScreenProbe probe;
            probe.position = vec4( 0.0 );
            probe.normal   = vec4( 0.0 );
            if ( dot( normal, normal ) > 0.001 )
            {
                const vec3 p   = ReconstructWorldPosition( uv, texelFetch( u_GBufferDepth, pixel, 0 ).r,
                                                           giFrame.invViewProjection );
                probe.position = vec4( p, max( distance( p, giFrame.camera.xyz ), 1e-3 ) );
                probe.normal   = vec4( normalize( normal ), SPPackPixel( pixel ) );
            }

            if ( level == 0u )
            {
                const uint id = SPUniformTileIndex( cell ); // uniform probe i is tile i
                screenProbes[id]                     = probe;
                screenProbeIndex[1u + id]            = probe.position.w > 0.0 ? id : SP_INVALID;
                for ( uint adaptive = 1u; adaptive < SP_LEVELS; ++adaptive )
                    screenProbeIndex[SPTileHeader( id, adaptive )] = 0u;
                return;
            }

            if ( probe.position.w <= 0.0 )
                return;
            uvec4 ids;
            vec4  weights;
            SPInterpolate( pixel, probe.position.xyz, probe.normal.xyz, probe.position.w, level, ids, weights );
            if ( dot( weights, vec4( 1.0 ) ) >= SP_ADAPTIVE_EPSILON )
                return;
            const uint k = atomicAdd( screenProbeIndex[0], 1u );
            if ( k >= screenProbeParams.grid.w )
                return; // the budget is spent: the integrate reads the world radiance cache here
            const uint id    = screenProbeParams.grid.z + k;
            screenProbes[id] = probe;
            const uint tile  = SPUniformTileIndex( cell >> int( level ) );
            const uint slot  = atomicAdd( screenProbeIndex[SPTileHeader( tile, level )], 1u );
            screenProbeIndex[SPTileSlot( tile, level, slot )] = id;
        }
    }
}

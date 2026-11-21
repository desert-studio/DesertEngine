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
        Uniform sampler PointClampSampler;
        Uniform(4) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(5) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        // UE ScreenProbeWorldSpeed (LumenScreenProbeGather.usf:97-109): each probe's world speed, cm per frame - its
        // surface's position minus where the velocity says it was last frame. The trace's moving test compares a
        // hit's speed against it (Common/ScreenProbeTracing.glslh SPIsTraceMoving).
        WriteBuffer(6) ScreenProbeSpeedUB
        {
            float screenProbeSpeed[];
        };
        Uniform(7) texture2D Velocity; // NDC.xyz current - previous (View/Velocity.hpp)
        #define Velocity sampler2D(Velocity, PointClampSampler)
        // FPS-SP-CHAIN-2 (UE ScreenProbeIndirectArgs): the dispatch of every one-group-a-probe pass (the screen
        // trace, the spatial filter, the filter; ScreenProbeGather.cpp ScreenProbes.PlacedArgs) - x = the uniform
        // probes plus the adaptive ones placed within the budget. Level 0 resets it to the uniform grid, each
        // adaptive probe raises it past its own id; w counts the adaptive probes wanted (the census).
        Buffer(8) ScreenProbePlacedArgsUB
        {
            uvec4 screenProbePlacedArgs;
        };

        PushConstant ScreenProbePlacePush
        {
            uint Level;
        };

        // FPS-SP-LIST-ATOMICS (UE BuildListsCS group reservation): an adaptive level's group counts its wanted probes
        // in shared memory and reserves their ids with ONE atomicAdd on the adaptive count, and raises the dispatch
        // and the census once - was an atomicAdd and two atomicMax per wanted probe on the same three words (the
        // per-tile headers stay per probe: each tile's own word). Which wanted probes the budget keeps was first
        // come and stays so; the ids' order is only the probes' slots (each tile's list names its own ids).
        shared uint s_Wanted;     // the group's wanted adaptive probes
        shared uint s_WantedBase; // the adaptive count before them: the first one's k

        // The level's probe at @p cell: @p probe / @p speed / @p pixel, false when the cell holds none to place
        // (past the grid, a coarser level's position, past the screen).
        bool SPPlaceProbe( const uint level, const ivec2 cell, out ivec2 pixel, out ScreenProbe probe, out float speed )
        {
            const vec2  extent = giFrame.screen.xy;
            const uvec2 grid   = SPGridSize( level, extent );
            probe.position = vec4( 0.0 );
            probe.normal   = vec4( 0.0 );
            speed          = 0.0;
            pixel          = ivec2( 0 );
            if ( any( greaterThanEqual( uvec2( cell ), grid ) ) )
                return false;
            if ( level > 0u && all( equal( cell & 1, ivec2( 0 ) ) ) )
                return false; // a coarser level's position

            pixel = SPProbePixel( level, cell, SPJitter() );
            if ( level == 0u )
                pixel = min( pixel, ivec2( extent ) - 1 );
            else if ( any( greaterThanEqual( pixel, ivec2( extent ) ) ) )
                return false;
            const vec2 uv     = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3 normal = texelFetch( GBufferBTexture, pixel, 0 ).xyz;
            if ( dot( normal, normal ) > 0.001 )
            {
                const float depth = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
                const vec3  p     = ReconstructWorldPosition( uv, depth, giFrame.invViewProjection );
                probe.position    = vec4( p, max( distance( p, giFrame.camera.xyz ), 1e-3 ) );
                probe.normal      = vec4( normalize( normal ), SPPackPixel( pixel ) );
                const vec3 previous = SPPreviousWorldPosition(
                     screenProbeParams,
                     SPPreviousSurfaceNdc( screenProbeParams, uv, depth, texelFetch( Velocity, pixel, 0 ).xyz ) );
                speed = length( p - previous );
            }
            return true;
        }

        void main()
        {
            const uint  level = Level;
            const ivec2 cell  = ivec2( gl_GlobalInvocationID.xy );
            if ( level == 0u && cell == ivec2( 0 ) )
            {
                screenProbeIndex[0]   = 0u;
                screenProbePlacedArgs = uvec4( screenProbeParams.grid.z, 1u, 1u, 0u );
            }
            ivec2       pixel;
            ScreenProbe probe;
            float       speed;
            const bool  placed = SPPlaceProbe( level, cell, pixel, probe, speed );

            if ( level == 0u ) // a push constant: level 0's dispatch returns here, before any barrier
            {
                if ( !placed )
                    return;
                const uint id = SPUniformTileIndex( cell ); // uniform probe i is tile i
                screenProbes[id]                     = probe;
                screenProbeSpeed[id]                 = speed;
                screenProbeIndex[1u + id]            = probe.position.w > 0.0 ? id : SP_INVALID;
                for ( uint adaptive = 1u; adaptive < SP_LEVELS; ++adaptive )
                    screenProbeIndex[SPTileHeader( id, adaptive )] = 0u;
                return;
            }

            if ( gl_LocalInvocationIndex == 0u )
                s_Wanted = 0u;
            barrier();
            bool wanted = placed && probe.position.w > 0.0;
            if ( wanted )
            {
                uvec4 ids;
                vec4  weights;
                SPInterpolate( pixel, probe.position.xyz, probe.normal.xyz, probe.position.w, level, ids, weights );
                wanted = dot( weights, vec4( 1.0 ) ) < SP_ADAPTIVE_EPSILON;
            }
            uint local = 0u;
            if ( wanted )
                local = atomicAdd( s_Wanted, 1u );
            barrier();
            if ( gl_LocalInvocationIndex == 0u && s_Wanted != 0u )
            {
                const uint count = s_Wanted;
                const uint base  = atomicAdd( screenProbeIndex[0], count );
                // w: the adaptive probes wanted, the budget's refusals included - the census of the dev counters
                // (ScreenProbeGather.cpp ScreenProbeCensusCounts); no dispatch reads w.
                atomicMax( screenProbePlacedArgs.w, base + count );
                // x: past the last id the budget keeps (the group's kept probes are k in [base, min(end, budget))).
                if ( base < screenProbeParams.grid.w )
                    atomicMax( screenProbePlacedArgs.x,
                               screenProbeParams.grid.z + min( base + count, screenProbeParams.grid.w ) );
                s_WantedBase = base;
            }
            barrier();
            if ( !wanted )
                return;
            const uint k = s_WantedBase + local;
            if ( k >= screenProbeParams.grid.w )
                return; // the budget is spent: the integrate reads the world radiance cache here
            const uint id    = screenProbeParams.grid.z + k;
            screenProbes[id]     = probe;
            screenProbeSpeed[id] = speed;
            const uint tile  = SPUniformTileIndex( cell >> int( level ) );
            const uint slot  = atomicAdd( screenProbeIndex[SPTileHeader( tile, level )], 1u );
            screenProbeIndex[SPTileSlot( tile, level, slot )] = id;
        }
    }
}

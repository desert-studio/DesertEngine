// DesertAsset {"Kind":"Shader","Guid":"940b3760fc734b088d9128944ada0771","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 1/4 — PLACE (UE ScreenProbeDownsampleDepthUniformCS + ScreenProbeAdaptivePlacementCS):
// one thread per tile of the level in the push constant. Level 0 puts a uniform probe on every 16-pixel tile's
// centre pixel that holds geometry and resets the adaptive count; levels 1 and 2 put an adaptive probe on an 8 / 4
// pixel tile's centre where the coarser probes' interpolation weight falls under SP_ADAPTIVE_WEIGHT_THRESHOLD,
// while the adaptive budget lasts. Every tile writes its grid entry, so the index needs no clear.
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
            const uint entry = SPGridOffset( level, extent ) + uint( cell.y ) * grid.x + uint( cell.x );

            const ivec2 pixel  = SPProbePixel( level, cell, extent );
            const vec2  uv     = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  normal = texelFetch( u_GBufferB, pixel, 0 ).xyz;
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
                const uint id = entry - 1u; // level 0's grid starts at 1: uniform probe i is cell i
                screenProbes[id]       = probe;
                screenProbeIndex[entry] = probe.position.w > 0.0 ? id : SP_INVALID;
                return;
            }

            uint id = SP_INVALID;
            if ( probe.position.w > 0.0 )
            {
                float covered = 0.0;
                for ( uint coarser = 0u; coarser < level; ++coarser )
                {
                    uvec4 ids;
                    vec4  weights;
                    SPLevelWeights( coarser, vec2( pixel ) + 0.5, probe.position.xyz, probe.normal.xyz,
                                    probe.position.w, extent, ids, weights );
                    covered += dot( weights, vec4( 1.0 ) );
                }
                if ( covered < SP_ADAPTIVE_WEIGHT_THRESHOLD )
                {
                    const uint k = atomicAdd( screenProbeIndex[0], 1u );
                    if ( k < screenProbeParams.grid.w )
                    {
                        id               = screenProbeParams.grid.z + k;
                        screenProbes[id] = probe;
                    }
                }
            }
            screenProbeIndex[entry] = id;
        }
    }
}

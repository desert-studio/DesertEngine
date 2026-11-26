// DesertAsset {"Kind":"Shader","Guid":"3752da27f25d4444887d8e258aa0ed4c","Versions":{"SHDR":1},"Dependencies":[]}
// THE RESTIR GI FINAL GATHER, 3/4 — SPATIAL RESAMPLING (Ouyang et al. 2021, alg. 4; UE 5.4+ LumenReSTIRGather.usf
// SpatialResamplingCS): the temporal reservoir streamed with ReSTIRParams.extent.z neighbours on a disk of
// tuning.y reservoir texels (a random rotation a texel and frame), each reused at this visible point only when
//   * its surface is this one (RESTIRSimilarSurface, as the temporal pass),
//   * the Jacobian of moving its sample here stays bounded (RESTIRJacobian: 0 rejects), its target re-evaluated
//     here and divided by it (p^' = p^(q at p) / J), and
//   * its sample is visible from here (GITraceVisibility up to the hit; a sky sample to the full trace distance).
// Biased M-sum combination (the paper's alg. 4 with visibility-tested candidates). Read only by the resolve: the
// history keeps the temporal reservoirs, so a neighbour's samples never feed back into the next frame's reuse.
// Compiled per tracer (GlobalIllumination::TracerVariant), the hardware permutations only.
Shader "ReSTIRGISpatial"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/ReSTIRGI.glslh>

        ReadBuffer(0) ReSTIRParamsUB
        {
            ReSTIRParams restirParams;
        };
        ReadBuffer(1) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(2) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(3) texture2D TemporalHit;
        #define TemporalHit sampler2D(TemporalHit, PointClampSampler)
        Uniform(4) texture2D TemporalRadiance;
        #define TemporalRadiance sampler2D(TemporalRadiance, PointClampSampler)
        Uniform(5) texture2D TemporalState;
        #define TemporalState sampler2D(TemporalState, PointClampSampler)
        layout(binding = 6, rgba32f) restrict writeonly uniform image2D ReSTIRHitOut;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D ReSTIRRadianceOut;
        layout(binding = 8, rgba32f) restrict writeonly uniform image2D ReSTIRStateOut;

        // 32..42: the tracer's own resources (visibility rays only: no hit is lit here).
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>

        #define RESTIR_TRACE_DISTANCE 20000.0 // Common/ScreenProbeTracing.glslh SP_TRACE_DISTANCE
        #define RESTIR_GOLDEN_ANGLE   2.39996323

        vec3 RESTIRVisiblePointAt( ivec2 pixel )
        {
            const vec2 uv = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            return ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                             giFrame.invViewProjection );
        }

        void main()
        {
            const ivec2 texel  = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 extent = ivec2( restirParams.extent.xy );
            if ( any( greaterThanEqual( texel, extent ) ) )
                return;
            const vec4 state = texelFetch( TemporalState, texel, 0 );
            if ( state.y <= 0.0 )
            {
                imageStore( ReSTIRHitOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRRadianceOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRStateOut, texel, vec4( 0.0 ) );
                return;
            }
            const uint  frame        = restirParams.frame.x;
            const ivec2 screen       = ivec2( giFrame.screen.xy );
            const vec3  p            = RESTIRVisiblePointAt( RESTIRReservoirPixel( texel, frame, screen ) );
            const vec3  n            = RESTIRUnpackNormal( floatBitsToUint( state.w ) );
            const float viewDistance = state.z;
            const GITraceBias bias   = GIRayBias( p, n );
            const vec3        origin = p + n * bias.nearOffset;

            const ReSTIRReservoir own =
                 RESTIRLoad( texelFetch( TemporalHit, texel, 0 ), texelFetch( TemporalRadiance, texel, 0 ), state );
            ReSTIRReservoir r = RESTIREmptyReservoir();
            RESTIRUpdate( r, own, RESTIRTargetPdf( own, p, n ) * own.W * own.M, RESTIRRandom( texel, frame, 4u ) );

            const float rotation = RESTIRRandom( texel, frame, 5u ) * 2.0 * RESTIR_PI;
            for ( uint k = 0u; k < restirParams.extent.z; ++k )
            {
                const float angle  = rotation + float( k ) * RESTIR_GOLDEN_ANGLE;
                const float radius = restirParams.tuning.y * sqrt( RESTIRRandom( texel, frame, 6u + k ) );
                const ivec2 q      = texel + ivec2( round( vec2( cos( angle ), sin( angle ) ) * radius ) );
                if ( q == texel || any( lessThan( q, ivec2( 0 ) ) ) || any( greaterThanEqual( q, extent ) ) )
                    continue;
                const vec4 qState = texelFetch( TemporalState, q, 0 );
                if ( qState.y <= 0.0 ||
                     !RESTIRSimilarSurface( viewDistance, n, qState.z,
                                            RESTIRUnpackNormal( floatBitsToUint( qState.w ) ), restirParams.tuning.z,
                                            restirParams.tuning.w ) )
                    continue;
                const ReSTIRReservoir candidate =
                     RESTIRLoad( texelFetch( TemporalHit, q, 0 ), texelFetch( TemporalRadiance, q, 0 ), qState );
                const vec3  pq       = RESTIRVisiblePointAt( RESTIRReservoirPixel( q, frame, screen ) );
                const float jacobian = RESTIRJacobian( candidate, pq, p );
                float       target   = jacobian > 0.0 ? RESTIRTargetPdf( candidate, p, n ) / jacobian : 0.0;
                if ( target > 0.0 )
                {
                    const bool  sky = dot( candidate.hitNormal, candidate.hitNormal ) < 0.5;
                    const vec3  dir = RESTIRSampleDirection( candidate, p );
                    const float maxT =
                         sky ? RESTIR_TRACE_DISTANCE : max( length( candidate.hitPosition - origin ) * 0.99, 0.0 );
                    if ( GITraceVisibility( origin, dir, GIFarFieldTMin( bias, n, dir ), maxT ) < 0.5 )
                        target = 0.0;
                }
                // A rejected candidate still adds its M (biased M-sum: the paper's alg. 4).
                RESTIRUpdate( r, candidate, target * candidate.W * candidate.M, RESTIRRandom( texel, frame, 16u + k ) );
            }
            RESTIRFinalize( r, p, n );

            imageStore( ReSTIRHitOut, texel,
                        vec4( r.hitPosition, uintBitsToFloat( RESTIRPackNormal( r.hitNormal ) ) ) );
            imageStore( ReSTIRRadianceOut, texel, vec4( r.radiance, 1.0 ) );
            imageStore( ReSTIRStateOut, texel, vec4( r.W, r.M, viewDistance, state.w ) );
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"59fc8df9aa684485bd48c1ebcd9370a6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RESTIR GI FINAL GATHER, 2/4 — TEMPORAL RESAMPLING (Ouyang et al. 2021, alg. 3; UE 5.4+ LumenReSTIRGather.usf
// TemporalResamplingCS): this frame's initial reservoir streamed with the view's previous reservoir of the same
// surface - the reservoir pixel reprojected by the velocity (UE GetHistoryScreenPosition), the nearest previous
// reservoir texel, kept only when its surface is this one (RESTIRSimilarSurface: distance to the camera and normal,
// UE's depth / normal rejection). The previous M is capped (ReSTIRParams.tuning.x, UE
// r.Lumen.ReSTIRGather.TemporalResampling.MaxHistory-like) so a sample whose lighting changed is replaced within a
// few frames, and a previous reservoir the initial pass re-traced and found changed (its validation ray: an
// occluder, a moved surface, a light switched or moved - RESTIR_HISTORY_DROP in InitialRadiance.a) is not reused
// at all, so the indirect light follows within two frames. Written into the view's reservoir history (ScreenProbeGatherHistory::Reservoir), which next frame
// reads back here; the spatial pass reads it this frame. Runs only on a frame WITH the view's previous reservoirs
// (ReSTIRGather::AddPasses; UE skips temporal resampling when !bIsHistoryValid): without them the initial pass
// writes the history itself and no previous texture is bound.
Shader "ReSTIRGITemporal"
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
        Uniform(3) texture2D Velocity;
        #define Velocity sampler2D(Velocity, PointClampSampler)
        Uniform(4) texture2D InitialHit;
        #define InitialHit sampler2D(InitialHit, PointClampSampler)
        Uniform(5) texture2D InitialRadiance;
        #define InitialRadiance sampler2D(InitialRadiance, PointClampSampler)
        Uniform(6) texture2D InitialState;
        #define InitialState sampler2D(InitialState, PointClampSampler)
        Uniform(7) texture2D PreviousHit;
        #define PreviousHit sampler2D(PreviousHit, PointClampSampler)
        Uniform(8) texture2D PreviousRadiance;
        #define PreviousRadiance sampler2D(PreviousRadiance, PointClampSampler)
        Uniform(9) texture2D PreviousState;
        #define PreviousState sampler2D(PreviousState, PointClampSampler)
        layout(binding = 10, rgba32f) restrict writeonly uniform image2D ReSTIRHitOut;
        layout(binding = 11, rgba16f) restrict writeonly uniform image2D ReSTIRRadianceOut;
        layout(binding = 12, rgba32f) restrict writeonly uniform image2D ReSTIRStateOut;

        void main()
        {
            const ivec2 texel = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 extent = ivec2( restirParams.extent.xy );
            if ( any( greaterThanEqual( texel, extent ) ) )
                return;
            const vec4 initialState = texelFetch( InitialState, texel, 0 );
            if ( initialState.y <= 0.0 )
            {
                imageStore( ReSTIRHitOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRRadianceOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRStateOut, texel, vec4( 0.0 ) );
                return;
            }
            const uint  frame        = restirParams.frame.x;
            const ivec2 pixel        = RESTIRReservoirPixel( texel, frame, ivec2( giFrame.screen.xy ) );
            const vec2  uv           = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const float deviceDepth  = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            const vec3  p            = ReconstructWorldPosition( uv, deviceDepth, giFrame.invViewProjection );
            const vec3  n            = RESTIRUnpackNormal( floatBitsToUint( initialState.w ) );
            const float viewDistance = initialState.z;

            const ReSTIRReservoir initial =
                 RESTIRLoad( texelFetch( InitialHit, texel, 0 ), texelFetch( InitialRadiance, texel, 0 ), initialState );
            ReSTIRReservoir r = RESTIREmptyReservoir();
            RESTIRUpdate( r, initial, RESTIRTargetPdf( initial, p, n ) * initial.W * initial.M,
                          RESTIRRandom( texel, frame, 2u ) );

            // The initial pass's verdict on the history reservoir this texel reprojects to (its validation ray,
            // RESTIRHistoryStillValid): dropped = the light or the occluder changed - M and W start over from this
            // frame's sample alone (UE's ReSTIR gather history validation).
            const bool historyKept = texelFetch( InitialRadiance, texel, 0 ).a > 0.5;
            if ( historyKept )
            {
                ivec2 previousTexel;
                if ( RESTIRPreviousTexel( uv, texelFetch( Velocity, pixel, 0 ).xy, restirParams.jitter,
                                          giFrame.screen.xy, extent, previousTexel ) )
                {
                    const vec4 previousState = texelFetch( PreviousState, previousTexel, 0 );
                    if ( previousState.y > 0.0 &&
                         RESTIRSimilarSurface( viewDistance, n, previousState.z,
                                               RESTIRUnpackNormal( floatBitsToUint( previousState.w ) ),
                                               restirParams.tuning.z, restirParams.tuning.w ) )
                    {
                        ReSTIRReservoir previous = RESTIRLoad( texelFetch( PreviousHit, previousTexel, 0 ),
                                                               texelFetch( PreviousRadiance, previousTexel, 0 ),
                                                               previousState );
                        previous.M = min( previous.M, restirParams.tuning.x );
                        RESTIRUpdate( r, previous, RESTIRTargetPdf( previous, p, n ) * previous.W * previous.M,
                                      RESTIRRandom( texel, frame, 3u ) );
                    }
                }
            }
            RESTIRFinalize( r, p, n );

            imageStore( ReSTIRHitOut, texel,
                        vec4( r.hitPosition, uintBitsToFloat( RESTIRPackNormal( r.hitNormal ) ) ) );
            imageStore( ReSTIRRadianceOut, texel, vec4( r.radiance, 1.0 ) );
            imageStore( ReSTIRStateOut, texel, vec4( r.W, r.M, viewDistance, initialState.w ) );
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"b84141a9929c450a980d12e8a9d0340f","Versions":{"SHDR":1},"Dependencies":[]}
// THE RESTIR GI FINAL GATHER, 1/4 — THE INITIAL SAMPLE (Ouyang et al. 2021, alg. 2; UE 5.4+
// LumenReSTIRGather.usf InitialSamplingCS): one cosine-distributed ray a reservoir texel, from the reservoir's pixel
// this frame (RESTIRReservoirPixel: one of its RESTIR_DOWNSAMPLE² pixels, rotating per frame), traced to the full
// distance by the hardware tracer (GITraceRay) and lit where it lands as the screen probes' cache-miss leg lights
// a hit (GIHitRadiance; the sky: GIMissRadiance). The sample becomes a one-candidate reservoir: M = 1,
// W = 1 / pdf. The reservoir layout: Common/ReSTIRGI.glslh. A pixel with no G-buffer normal (the sky) writes M = 0.
// Compiled per tracer (GlobalIllumination::TracerVariant), the hardware permutations only
// (GlobalIllumination::ReSTIRGatherRuns), each twice: with the history's validation rays, and RESTIR_INITIAL_NO_HISTORY
// for a frame without the view's previous reservoirs (it then writes the reservoir history itself).
Shader "ReSTIRGIInitial"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/ReSTIRGI.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

        ReadBuffer(0) ReSTIRParamsUB
        {
            ReSTIRParams restirParams;
        };
        ReadBuffer(1) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(2) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(3) SpotLightsUB
        {
            SpotLight spotLights[];
        };
        ReadBuffer(4) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(5) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(6) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(7) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(8) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };

        // The hit's lighting reads exactly what the screen probes' cache-miss leg reads (ScreenProbeTrace.shader
        // SP_TRACE_LIGHTS_HITS / SP_TRACE_READS_CACHE): the G-buffer (GIHitLighting.glslh's screen tolerance and
        // EMISSION_IS_LIGHT), the sky and the radiance cache's bounce.
        Uniform sampler PointClampSampler;
        Uniform sampler LinearClampSampler;
        Uniform(9) textureCube EnvironmentSpecularTexture;
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        Uniform(10) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(11) texture2D GBufferATexture;
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        Uniform(12) texture2D GBufferEmissiveTexture;
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, PointClampSampler)
        Uniform(13) utexture2D GBufferShadingWordTexture;
        #define GBufferShadingWordTexture usampler2D(GBufferShadingWordTexture, PointClampSampler)
        Uniform(14) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(15) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        Uniform(16) texture2D RadianceCacheRadianceAtlasTexture;
        #define RadianceCacheRadianceAtlasTexture sampler2D(RadianceCacheRadianceAtlasTexture, LinearClampSampler)
        Uniform(17) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        layout(binding = 18, rgba32f) restrict writeonly uniform image2D ReSTIRHitOut;
        layout(binding = 19, rgba16f) restrict writeonly uniform image2D ReSTIRRadianceOut;
        layout(binding = 20, rgba32f) restrict writeonly uniform image2D ReSTIRStateOut;
        // The validation ray's inputs: the view's previous reservoirs, found as ReSTIRGITemporal finds them. Not
        // declared in the RESTIR_INITIAL_NO_HISTORY permutation (a frame without them: this pass writes the view's
        // reservoir history itself, no temporal pass runs - ReSTIRGather::AddPasses), so nothing stands in for them.
        #ifndef RESTIR_INITIAL_NO_HISTORY
        Uniform(21) texture2D Velocity;
        #define Velocity sampler2D(Velocity, PointClampSampler)
        Uniform(22) texture2D PreviousHit;
        #define PreviousHit sampler2D(PreviousHit, PointClampSampler)
        Uniform(23) texture2D PreviousRadiance;
        #define PreviousRadiance sampler2D(PreviousRadiance, PointClampSampler)
        Uniform(24) texture2D PreviousState;
        #define PreviousState sampler2D(PreviousState, PointClampSampler)
        #endif

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #define RADIANCE_CACHE_RADIANCE_ATLAS RadianceCacheRadianceAtlasTexture
        #include <Common/RadianceCache.glslh>
        // 25..31: the surface cache a hit reads first; 32..42: the tracer's own resources.
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>

        // The sample's one cone: a pixel's ray (UE ReSTIR GI traces a thin ray; the surface cache picks its finest mip).
        #define RESTIR_RAY_CONE_ANGLE 0.02
        // The screen probes' full trace distance (Common/ScreenProbeTracing.glslh SP_TRACE_DISTANCE).
        #define RESTIR_TRACE_DISTANCE 20000.0

        // THE HISTORY'S VALIDATION RAY (UE's ReSTIR gather history validation; Ouyang 2021 §5 "stale samples"):
        // the history reservoir the temporal pass will reuse for this texel (RESTIRPreviousTexel, the same surface
        // test) is re-traced from this frame's visible point toward its stored hit and lit again there. A changed
        // hit or a changed light drops it (RESTIR_HISTORY_DROP); a history the temporal pass would not reuse anyway
        // (off screen, another surface, empty, below this surface) is kept untouched.
        #ifndef RESTIR_INITIAL_NO_HISTORY
        float ValidateHistory( ivec2 pixel, vec2 uv, vec3 p, vec3 n, GITraceBias bias )
        {
            ivec2 previousTexel;
            if ( !RESTIRPreviousTexel( uv, texelFetch( Velocity, pixel, 0 ).xy, restirParams.jitter,
                                       giFrame.screen.xy, ivec2( restirParams.extent.xy ), previousTexel ) )
                return RESTIR_HISTORY_KEEP;
            const vec4 previousState = texelFetch( PreviousState, previousTexel, 0 );
            if ( previousState.y <= 0.0 ||
                 !RESTIRSimilarSurface( length( p - giFrame.camera.xyz ), n, previousState.z,
                                        RESTIRUnpackNormal( floatBitsToUint( previousState.w ) ),
                                        restirParams.tuning.z, restirParams.tuning.w ) )
                return RESTIR_HISTORY_KEEP;
            const ReSTIRReservoir history = RESTIRLoad( texelFetch( PreviousHit, previousTexel, 0 ),
                                                        texelFetch( PreviousRadiance, previousTexel, 0 ),
                                                        previousState );
            const vec3  origin   = p + n * bias.nearOffset;
            const vec3  toHit    = history.hitPosition - origin;
            const float expected = length( toHit );
            const vec3  dir      = toHit / max( expected, 1e-4 );
            if ( dot( n, dir ) <= 0.0 )
                return RESTIR_HISTORY_KEEP; // its target function is zero here: the temporal pass weighs it 0
            const bool  sky   = dot( history.hitNormal, history.hitNormal ) < 0.5;
            const float slack = bias.nearOffset + bias.farOffset;

            GITraceInput ray;
            ray.origin    = origin;
            ray.tMin      = 0.0;
            ray.farTMin   = GIFarFieldTMin( bias, n, dir );
            ray.direction = dir;
            ray.tMax      = sky ? RESTIR_TRACE_DISTANCE : expected * ( 1.0 + restirParams.validation.y ) + slack;
            ray.coneAngle = RESTIR_RAY_CONE_ANGLE;
            ray.flags     = 0u;
            const GITraceResult result = GITraceRay( ray );
            if ( !RESTIRHistoryHitUnchanged( history, origin, result.hit, result.surface.distance,
                                             restirParams.validation, slack ) )
                return RESTIR_HISTORY_DROP;
            const vec3 radianceNow = RESTIRClampRadiance(
                 max( result.hit ? GIHitRadiance( result.surface, dir ) : GIMissRadiance( dir ), vec3( 0.0 ) ),
                 restirParams.rayClamp.x );
            return RESTIRHistoryLightingUnchanged( history, radianceNow, restirParams.validation )
                        ? RESTIR_HISTORY_KEEP
                        : RESTIR_HISTORY_DROP;
        }
        #endif

        void main()
        {
            const ivec2 texel = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( uvec2( texel ), restirParams.extent.xy ) ) )
                return;
            const uint  frame = restirParams.frame.x;
            const ivec2 pixel = RESTIRReservoirPixel( texel, frame, ivec2( giFrame.screen.xy ) );
            const vec4  gb    = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( ReSTIRHitOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRRadianceOut, texel, vec4( 0.0 ) );
                imageStore( ReSTIRStateOut, texel, vec4( 0.0 ) ); // M = 0: no surface, no sample
                return;
            }
            const vec3 n     = normalize( gb.xyz );
            const vec2 uv    = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3 p     = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                         giFrame.invViewProjection );
            const vec2 xi    = vec2( RESTIRRandom( texel, frame, 0u ), RESTIRRandom( texel, frame, 1u ) );
            const vec3 dir   = RESTIRCosineDirection( n, xi );
            const float pdf  = max( dot( n, dir ), 1e-4 ) / RESTIR_PI;

            const GITraceBias bias = GIRayBias( p, n );
            GITraceInput      ray;
            ray.origin    = p + n * bias.nearOffset;
            ray.tMin      = 0.0;
            ray.farTMin   = GIFarFieldTMin( bias, n, dir );
            ray.direction = dir;
            ray.tMax      = RESTIR_TRACE_DISTANCE;
            ray.coneAngle = RESTIR_RAY_CONE_ANGLE;
            ray.flags     = 0u;
            const GITraceResult result = GITraceRay( ray );

            vec3 radiance;
            vec3 hitPosition;
            vec3 hitNormal;
            if ( result.hit )
            {
                radiance    = GIHitRadiance( result.surface, dir );
                hitPosition = result.surface.position;
                hitNormal   = result.surface.normal;
            }
            else
            {
                radiance    = GIMissRadiance( dir );
                hitPosition = p + dir * RESTIR_SKY_DISTANCE;
                hitNormal   = vec3( 0.0 );
            }
            radiance = RESTIRClampRadiance( max( radiance, vec3( 0.0 ) ), restirParams.rayClamp.x );

            // This frame's checkerboard half re-traces its history (RESTIRValidatesThisFrame); the verdict rides in
            // Radiance.a to ReSTIRGITemporal. Without a history this reservoir IS the history (Radiance.a = 1, as
            // the temporal pass writes it).
            #ifdef RESTIR_INITIAL_NO_HISTORY
            const float verdict = RESTIR_HISTORY_KEEP;
            #else
            const float verdict = RESTIRValidatesThisFrame( texel, frame ) ? ValidateHistory( pixel, uv, p, n, bias )
                                                                           : RESTIR_HISTORY_KEEP;
            #endif

            imageStore( ReSTIRHitOut, texel, vec4( hitPosition, uintBitsToFloat( RESTIRPackNormal( hitNormal ) ) ) );
            imageStore( ReSTIRRadianceOut, texel, vec4( radiance, verdict ) );
            imageStore( ReSTIRStateOut, texel,
                        vec4( 1.0 / pdf, 1.0, length( p - giFrame.camera.xyz ),
                              uintBitsToFloat( RESTIRPackNormal( n ) ) ) );
        }
    }
}

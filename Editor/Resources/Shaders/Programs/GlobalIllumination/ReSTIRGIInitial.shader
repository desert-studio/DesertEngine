// DesertAsset {"Kind":"Shader","Guid":"b84141a9929c450a980d12e8a9d0340f","Versions":{"SHDR":1},"Dependencies":[]}
// THE RESTIR GI FINAL GATHER, 1/4 — THE INITIAL SAMPLE (Ouyang et al. 2021, alg. 2; UE 5.4+
// LumenReSTIRGather.usf InitialSamplingCS): one cosine-distributed ray a reservoir texel, from the reservoir's pixel
// this frame (RESTIRReservoirPixel: one of its RESTIR_DOWNSAMPLE² pixels, rotating per frame), traced to the full
// distance by the hardware tracer (GITraceRay) and lit where it lands as the screen probes' cache-miss leg lights
// a hit (GIHitRadiance; the sky: GIMissRadiance). The sample becomes a one-candidate reservoir: M = 1,
// W = 1 / pdf. The reservoir layout: Common/ReSTIRGI.glslh. A pixel with no G-buffer normal (the sky) writes M = 0.
// Compiled per tracer (GlobalIllumination::TracerVariant), the hardware permutations only
// (GlobalIllumination::ReSTIRGatherRuns).
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

            imageStore( ReSTIRHitOut, texel, vec4( hitPosition, uintBitsToFloat( RESTIRPackNormal( hitNormal ) ) ) );
            imageStore( ReSTIRRadianceOut, texel, vec4( radiance, 1.0 ) );
            imageStore( ReSTIRStateOut, texel,
                        vec4( 1.0 / pdf, 1.0, length( p - giFrame.camera.xyz ),
                              uintBitsToFloat( RESTIRPackNormal( n ) ) ) );
        }
    }
}

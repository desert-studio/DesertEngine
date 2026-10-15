// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 2/4 — TRACE (UE ScreenProbeTraceMeshSDFsCS / ScreenProbeTraceVoxelsCS with the radiance
// cache's far field, LumenScreenProbeTracing.usf): one workgroup per probe, one ray per hemi-octahedral texel,
// jittered inside its texel every frame. The ray goes through the GI ray contract (Common/GITracing.glslh: the
// permutation's tracer, GIHitRadiance on a hit) only as far as the world radiance cache's coverage asks
// (minTraceDistanceBeforeCache: past it the cache's probes see what the ray would); a ray that gets there without a
// hit reads the cache's RADIANCE in its direction at the probe (RadianceCacheRadianceAt, UE
// SampleRadianceCacheInterpolated). Outside the cache, or where its probes have not traced yet, the ray goes on to
// the full trace distance and the sky. The program is compiled per tracer (GlobalIllumination::TracerVariant);
// it names none. .a keeps the distance the ray's radiance came from (the filter's angle test).
Shader "ScreenProbeTrace"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

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
        ReadBuffer(4) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(5) SpotLightsUB
        {
            SpotLight spotLights[];
        };
        ReadBuffer(6) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(7) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(8) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(9) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(10) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };

        // 11..16: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        Uniform(17) samplerCube u_EnvSpecularTex;
        Uniform(18) sampler2D u_GBufferDepth;
        Uniform(19) sampler2D u_GBufferA;
        Uniform(20) sampler2D u_GBufferEmissive;
        Uniform(21) sampler2D u_RadianceCacheIrradiance;
        Uniform(22) sampler2D u_RadianceCacheDepth;
        Uniform(23) sampler2D u_RadianceCacheRadianceAtlas;
        layout(binding = 24, rgba16f) restrict writeonly uniform image2D u_ScreenProbeTraced;

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #define RADIANCE_CACHE_RADIANCE_ATLAS u_RadianceCacheRadianceAtlas
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 11
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>

        // How far a ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance; the radiance cache's own).
        #define SP_TRACE_DISTANCE 20000.0

        // Per texel and frame: a jitter inside the texel and a dither (PCG hash).
        vec3 TexelNoise( uint probe, uint texel, uint frame )
        {
            uvec3 v = uvec3( probe, texel, frame ) * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            return vec3( v & 0xFFFFu ) / 65536.0;
        }

        void main()
        {
            const uint probe = gl_WorkGroupID.x;
            const uint uniformProbes = screenProbeParams.grid.z;
            if ( probe >= uniformProbes &&
                 probe - uniformProbes >= min( screenProbeIndex[0], screenProbeParams.grid.w ) )
                return;
            const ScreenProbe state = screenProbes[probe];
            if ( state.position.w <= 0.0 )
                return;

            const uint  texel  = gl_LocalInvocationIndex;
            const ivec2 t      = ivec2( texel % SP_RES, texel / SP_RES );
            const vec3  noise  = TexelNoise( probe, texel, screenProbeParams.atlas.y );
            const vec2  o      = ( vec2( t ) + noise.xy ) / float( SP_RES ) * 2.0 - 1.0;
            const vec3  N      = state.normal.xyz;
            const vec3  dir    = SPProbeFrame( N ) * SPHemiOctDecode( o );
            const vec3  origin = state.position.xyz + N * GIRayBias( state.position.xyz, N, dir ).normalOffset;

            GITraceInput ray;
            ray.origin    = origin;
            ray.tMin      = 0.0;
            ray.direction = dir;
            ray.tMax      = SP_TRACE_DISTANCE;
            // The cone of one texel: the hemisphere's 2 pi over SP_RES^2 texels, as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( SP_RES * SP_RES ) );
            ray.flags     = 0u;

            const RadianceCacheCoverage coverage = RadianceCacheCoverageAt( radianceCache, origin, noise.z );
            if ( coverage.valid )
                ray.tMax = min( coverage.minTraceDistanceBeforeCache, SP_TRACE_DISTANCE );

            vec4                result;
            const GITraceResult near = GITraceRay( ray );
            if ( near.hit )
                result = vec4( GIHitRadiance( near.surface, dir ), near.surface.distance );
            else
            {
                const vec4 far = coverage.valid ? RadianceCacheRadianceAt( origin, dir, noise.z ) : vec4( 0.0, 0.0, 0.0, -1.0 );
                if ( far.a > 0.0 )
                    result = vec4( far.rgb, SP_TRACE_DISTANCE );
                else
                {
                    // No cache answer: the rest of the ray traced here, so a new cell is neither black nor sky.
                    ray.tMin = ray.tMax;
                    ray.tMax = SP_TRACE_DISTANCE;
                    const GITraceResult rest = coverage.valid ? GITraceRay( ray ) : near;
                    result = rest.hit ? vec4( GIHitRadiance( rest.surface, dir ), rest.surface.distance )
                                      : vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE );
                }
            }
            imageStore( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t, result );
        }
    }
}

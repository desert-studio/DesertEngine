// DesertAsset {"Kind":"Shader","Guid":"ec2cf83ab7234b04a0239d55a04ed53a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S RADIOSITY TRACE (GI-SHADE-DARK; UE LumenRadiosity.usf LumenRadiosityDistanceFieldTracingCS /
// the hardware RayGen, with PlaceProbes folded in): every page relit this frame (SceneFrame::LightingPages) is drawn
// into the radiosity trace atlas, one fragment per atlas texel. A texel is one ray of its tile's probe
// (Common/SurfaceCacheRadiosity.glslh): the probe stands on the tile's first opaque texel, the ray leaves it in the
// texel's stratum of the probe normal's hemisphere (jittered per frame) through the GI ray contract - the frame's
// tracer, a hit read from the cards' FinalLighting (the previous frame's, one bounce more per frame as UE's
// radiosity), a miss the sky (GIMissRadiance; UE: the skylight leaking into a miss). The result is blended with the
// texel's history by the pipeline (src alpha = the new sample's weight; 1 on a page captured this frame or after a
// full relight, UE's temporal accumulation otherwise).
// The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none.
Shader "SurfaceCacheRadiosityTrace"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(1) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(7) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        layout(location = 0) out flat uint v_CardIndex;
        layout(location = 1) out flat vec4 v_CardUVRect;
        layout(location = 2) out flat vec4 v_AtlasUVRect;
        layout(location = 3) out flat uint v_Captured;

        void main()
        {
            const SurfaceCardPage page    = surfaceCardPages[lightingPages[gl_InstanceIndex]];
            const vec2            corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                                       vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
            const vec2 uv = page.physicalAtlasUVRect.xy + corners[gl_VertexIndex] * page.physicalAtlasUVRect.zw;
            v_CardIndex   = page.card.x;
            v_CardUVRect  = page.cardUVRect;
            v_AtlasUVRect = page.physicalAtlasUVRect;
            v_Captured    = page.card.z;
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/GIFrame.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

        ReadBuffer(9) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(10) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(11) SpotLightsUB
        {
            SpotLight spotLights[];
        };
        ReadBuffer(12) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(13) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>

        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(14) textureCube EnvironmentSpecularTexture;
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        Uniform(15) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(16) texture2D GBufferATexture;
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        Uniform(17) texture2D GBufferEmissiveTexture;
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, PointClampSampler)

        // The world radiance cache, read by a hit no card holds (GIHitRadianceLit's bounce).
        #include <Common/RadianceCache.glslh>
        ReadBuffer(18) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(19) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(20) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(21) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(22) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        // The probe's normal (the Normal layer); its depth and opacity are the hit reads' (below).
        Uniform(8) texture2D AtlasNormalTexture;
        #define AtlasNormalTexture sampler2D(AtlasNormalTexture, PointClampSampler)

        // 32..42: the tracer's own resources (Common/GITracer.glslh).
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        // 0..6: the cards a hit reads first - and the card records, Depth and Opacity the probes stand on. Included
        // ahead of the hit lighting so that a permutation lighting hits in place still declares them.
        #define GI_SURFACE_CACHE_BINDING 0
        // GI-EMISSIVE-LIGHTS: under either tracer the direct lighting samples the emissive triangles as
        // lights (SurfaceCacheDirectLighting.shader), so the hits here leave their emission out - the card's
        // Emissive layer (binding 23) subtracted from its FinalLighting, a hit lit in place without its emissive.
        #define GI_HIT_EXCLUDES_EMISSION
        layout(binding = 23) uniform texture2D SurfaceCacheEmissiveTexture;
        #define SurfaceCacheEmissiveTexture sampler2D(SurfaceCacheEmissiveTexture, PointClampSampler)
        #include <Common/SurfaceCacheSampling.glslh>
        #include <Common/GIHitLighting.glslh>

        #define RADIOSITY_OPACITY SurfaceCacheOpacityTexture
        #include <Common/SurfaceCacheRadiosity.glslh>

        PushConstant SurfaceRadiosityPush
        {
            uvec4 RadiosityFrame; // x = frame index (the jitter), y = 1: no history (a full relight)
            vec4  RadiosityWeights; // x = the new sample's weight against the history (1 / frames accumulated)
        };

        // How far a radiosity ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance, as the cache's probes).
        #define RADIOSITY_TRACE_DISTANCE 20000.0

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        layout(location = 3) in flat uint v_Captured;
        Out(0) vec4 oRadiance;

        // Per texel and frame, a jitter inside the texel's stratum (PCG hash).
        vec2 StratumJitter( uvec2 texel, uint frame )
        {
            uvec3 v = uvec3( texel, frame ) * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x;
            return vec2( v.xy & 0xFFFFu ) / 65536.0;
        }

        void main()
        {
            const float weight = ( v_Captured != 0u || RadiosityFrame.y != 0u ) ? 1.0 : RadiosityWeights.x;
            const ivec2 atlasTexel = ivec2( gl_FragCoord.xy );
            const ivec4 page   = RadiosityPageTexels( v_AtlasUVRect, vec2( textureSize( SurfaceCacheOpacityTexture, 0 ) ) );
            const ivec2 inPage = atlasTexel - page.xy;
            const ivec4 tile   = RadiosityTileOf( inPage / RADIOSITY_PROBE_SPACING, page.zw );
            ivec2 probeTexel;
            if ( !RadiosityProbeTexel( page.xy + tile.xy, tile.zw, probeTexel ) )
            {
                oRadiance = vec4( 0.0, 0.0, 0.0, 1.0 );
                return;
            }
            const SurfaceCard card = surfaceCards[v_CardIndex];
            const vec3 position = RadiosityTexelWorldPosition( card, v_CardUVRect, page, probeTexel,
                                                               texelFetch( SurfaceCacheDepthTexture, probeTexel, 0 ).r );
            const vec3 N = RadiosityOctahedronToUnitVector( texelFetch( AtlasNormalTexture, probeTexel, 0 ).rg * 2.0 - 1.0 );

            const ivec2 stratum = inPage - tile.xy;
            const vec2  u = ( vec2( stratum ) + StratumJitter( uvec2( atlasTexel ), RadiosityFrame.x ) ) / vec2( tile.zw );
            const vec3  direction = RadiosityHemisphereDirection( u, N );

            const GITraceBias bias = GIRayBias( position, N, direction );
            GITraceInput ray;
            ray.origin    = position + N * bias.nearOffset;
            ray.tMin      = 0.0;
            ray.direction = direction;
            ray.tMax      = RADIOSITY_TRACE_DISTANCE;
            // The cone of one stratum: its solid angle 2 pi / count as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( tile.z * tile.w ) );
            ray.flags     = 0u;
            ray.farTMin   = GIFarFieldTMin( bias, N, direction );
            vec3 radiance = GIRayRadiance( ray );
            // A NaN or negative sample would stay in the history for good.
            if ( any( isnan( radiance ) ) || any( isinf( radiance ) ) )
                radiance = vec3( 0.0 );
            oRadiance = vec4( max( radiance, vec3( 0.0 ) ), weight );
        }
    }
}

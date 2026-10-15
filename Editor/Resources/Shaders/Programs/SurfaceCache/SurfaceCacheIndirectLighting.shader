// DesertAsset {"Kind":"Shader","Guid":"5c7221306c9643589755dfc28bebcaa7","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S INDIRECT LIGHTING (GI-CARD-LOOKUP; UE's radiosity slot, LumenRadiosity.cpp + the combine of
// LumenSceneLighting.usf): every page relit this frame (the same SceneFrame::LightingPages the direct lighting
// relit) takes the world radiance cache's irradiance at each texel (Common/RadianceCache.glslh
// RadianceCacheIrradianceAt, the update of this frame) as its IndirectLighting, and recombines FinalLighting =
// albedo / PI x direct + albedo x indirect + emissive (the cache's irradiance is already / PI). A traced hit then
// reads its bounce from the card in one fetch instead of interpolating eight cache probes per ray.
//
// WHERE THIS DIFFERS FROM UE (for the owner): UE's radiosity gathers each card texel's hemisphere by tracing
// probes against the previous FinalLighting (r.LumenScene.Radiosity.*). Here the texel reads the world radiance
// cache, whose probes trace the same cards - the same feedback (every frame adds a bounce) without a second set of
// probes; the radiosity probes (RadiosityProbeSpacing / HemisphereResolution of the budget) replace it when that
// task lands.
//
// Runs after the radiance cache's update of the frame (SceneRenderer::AddFrameTracedGI); the frame's traced hits
// read the previous FinalLighting (one frame of lag, as UE's radiosity has).
Shader "SurfaceCacheIndirectLighting"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(1) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        layout(location = 0) out flat uint v_CardIndex;
        layout(location = 1) out flat vec4 v_CardUVRect;
        layout(location = 2) out flat vec4 v_AtlasUVRect;

        void main()
        {
            const SurfaceCardPage page    = surfaceCardPages[lightingPages[gl_InstanceIndex]];
            const vec2            corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                                       vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
            const vec2 uv = page.physicalAtlasUVRect.xy + corners[gl_VertexIndex] * page.physicalAtlasUVRect.zw;
            v_CardIndex   = page.card.x;
            v_CardUVRect  = page.cardUVRect;
            v_AtlasUVRect = page.physicalAtlasUVRect;
            gl_Position   = vec4(ScreenUVToNdc(uv), 0.0, 1.0);
        }
    }
    Fragment
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/RadianceCache.glslh>
        ReadBuffer(2) SurfaceCardBuffer
        {
            SurfaceCard surfaceCards[];
        };
        Uniform(3) sampler2D u_AtlasDepth;
        Uniform(4) sampler2D u_AtlasAlbedo;
        Uniform(5) sampler2D u_AtlasOpacity;
        Uniform(6) sampler2D u_AtlasNormal;
        Uniform(7) sampler2D u_AtlasEmissive;
        Uniform(8) sampler2D u_AtlasDirectLighting;
        ReadBuffer(9) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(10) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(11) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(12) sampler2D u_RadianceCacheIrradiance;
        Uniform(13) sampler2D u_RadianceCacheDepth;
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        Out(0) vec4 oIndirect;
        Out(1) vec4 oFinal;

        // [-1, 1]² octahedral -> unit vector (UE OctahedronToUnitVector), as the direct lighting decodes it.
        vec3 OctahedronToUnitVector(vec2 o)
        {
            vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
            const float t = max(-n.z, 0.0);
            n.x += n.x >= 0.0 ? -t : t;
            n.y += n.y >= 0.0 ? -t : t;
            return normalize(n);
        }

        void main()
        {
            const ivec2 atlasTexel = ivec2(gl_FragCoord.xy);
            if (texelFetch(u_AtlasOpacity, atlasTexel, 0).r < 0.5)
            {
                oIndirect = vec4(0.0);
                oFinal    = vec4(0.0);
                return;
            }
            // The texel's world position and normal, exactly as the direct lighting places them.
            const SurfaceCard card = surfaceCards[v_CardIndex];
            const vec2  res        = vec2(uvec2(1u << (card.info.x & 0xFFu), 1u << ((card.info.x >> 8u) & 0xFFu)));
            const vec2  atlasSize  = vec2(textureSize(u_AtlasOpacity, 0));
            const ivec2 pageOrigin = ivec2(v_AtlasUVRect.xy * atlasSize + 0.5);
            const vec2  cardTexel  = floor(v_CardUVRect.xy * res + 0.5) + vec2(atlasTexel - pageOrigin) + 0.5;
            const vec2  uv         = cardTexel / res;
            const vec2  local      = vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y) * card.localExtent.xy;
            const vec3  origin = vec3(card.worldToLocal[0].w, card.worldToLocal[1].w, card.worldToLocal[2].w);
            const float depth  = texelFetch(u_AtlasDepth, atlasTexel, 0).r;
            const vec3 worldPos = origin + card.worldToLocal[0].xyz * local.x + card.worldToLocal[1].xyz * local.y +
                                  card.worldToLocal[2].xyz * (card.localExtent.z - depth * 2.0 * card.localExtent.z);
            const vec3 N = OctahedronToUnitVector(texelFetch(u_AtlasNormal, atlasTexel, 0).rg * 2.0 - 1.0);

            // The texel is seen from its own normal (no viewer): the cache's surface bias leaves along it.
            const vec4 irradiance = RadianceCacheIrradianceAt(worldPos, N, N, 0.5);
            const vec3 indirect   = irradiance.a > 0.0 ? max(irradiance.rgb, vec3(0.0)) : vec3(0.0);

            const vec3 albedo   = texelFetch(u_AtlasAlbedo, atlasTexel, 0).rgb;
            const vec3 emissive = texelFetch(u_AtlasEmissive, atlasTexel, 0).rgb;
            const vec3 direct   = texelFetch(u_AtlasDirectLighting, atlasTexel, 0).rgb;
            oIndirect = vec4(indirect, 0.0);
            oFinal    = vec4(albedo / 3.14159265 * direct + albedo * indirect + emissive, 0.0);
        }
    }
}

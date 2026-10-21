// DesertAsset {"Kind":"Shader","Guid":"5c7221306c9643589755dfc28bebcaa7","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S INDIRECT LIGHTING (GI-CARD-LOOKUP; UE's radiosity slot, LumenRadiosity.cpp + the combine of
// LumenSceneLighting.usf): every page relit this frame (the same SceneFrame::LightingPages the direct lighting
// relit) takes the world radiance cache's irradiance at each texel (Common/RadianceCache.glslh
// RadianceCacheIrradianceAt, the update of this frame) and leaves IndirectLighting = albedo x that irradiance (the
// cache's irradiance is already / PI) - plus, on a subsurface texel (the Subsurface layer), subsurface x the
// cache's irradiance at -N: the sky and bounce let through a leaf from its far side (FOLIAGE-LOOK-5, as the
// direct lighting lets the sun through) - and recombines FinalLighting = direct + indirect + emissive. A traced hit
// then reads its bounce from the card in one fetch instead of interpolating eight cache probes per ray.
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
        Uniform sampler u_LinearClampSampler;
        Uniform sampler u_PointClampSampler;
        Uniform(3) texture2D u_AtlasDepth;
        #define u_AtlasDepth sampler2D(u_AtlasDepth, u_PointClampSampler)
        Uniform(4) texture2D u_AtlasAlbedo;
        #define u_AtlasAlbedo sampler2D(u_AtlasAlbedo, u_PointClampSampler)
        Uniform(5) texture2D u_AtlasOpacity;
        #define u_AtlasOpacity sampler2D(u_AtlasOpacity, u_PointClampSampler)
        Uniform(6) texture2D u_AtlasNormal;
        #define u_AtlasNormal sampler2D(u_AtlasNormal, u_PointClampSampler)
        Uniform(7) texture2D u_AtlasEmissive;
        #define u_AtlasEmissive sampler2D(u_AtlasEmissive, u_PointClampSampler)
        Uniform(8) texture2D u_AtlasDirectLighting;
        #define u_AtlasDirectLighting sampler2D(u_AtlasDirectLighting, u_PointClampSampler)
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
        Uniform(12) texture2D u_RadianceCacheIrradiance;
        #define u_RadianceCacheIrradiance sampler2D(u_RadianceCacheIrradiance, u_LinearClampSampler)
        Uniform(13) texture2D u_RadianceCacheDepth;
        #define u_RadianceCacheDepth sampler2D(u_RadianceCacheDepth, u_LinearClampSampler)
        // The light the texel lets through (rgb); 0 = opaque (SurfaceCacheCardCopy).
        Uniform(14) texture2D u_AtlasSubsurface;
        #define u_AtlasSubsurface sampler2D(u_AtlasSubsurface, u_PointClampSampler)
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
            const vec3 albedo     = texelFetch(u_AtlasAlbedo, atlasTexel, 0).rgb;
            vec3       indirect   = irradiance.a > 0.0 ? albedo * max(irradiance.rgb, vec3(0.0)) : vec3(0.0);
            // The far side of a leaf, read the same way from -N.
            const vec3 subsurface = texelFetch(u_AtlasSubsurface, atlasTexel, 0).rgb;
            if (max(subsurface.r, max(subsurface.g, subsurface.b)) > 0.0)
            {
                const vec4 back = RadianceCacheIrradianceAt(worldPos, -N, -N, 0.5);
                if (back.a > 0.0)
                    indirect += subsurface * max(back.rgb, vec3(0.0));
            }

            const vec3 emissive = texelFetch(u_AtlasEmissive, atlasTexel, 0).rgb;
            const vec3 direct   = texelFetch(u_AtlasDirectLighting, atlasTexel, 0).rgb;
            oIndirect = vec4(indirect, 0.0);
            oFinal    = vec4(direct + indirect + emissive, 0.0);
        }
    }
}

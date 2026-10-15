// DesertAsset {"Kind":"Shader","Guid":"edbbc1bc44dd4359ad6ac3c780139967","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S DIRECT LIGHTING (GI-SURFACE-CACHE-4; UE LumenSceneDirectLighting.cpp/.usf + the combine of
// LumenSceneLighting.usf): every page picked this frame (SurfaceCacheScene's update budget, every page captured
// this frame) is relit texel by texel. A texel's world position is its card texel placed back at its captured depth
// (the card box: the card plane, minus the depth layer times the box depth), its normal the captured octahedral
// normal. It receives the SAME light the deferred composite shades the frame with: the sun with its cascades and
// the cloud layer, every point and spot light of the view's light buffers with its local shadow views - through the
// composite's own receiver texts (Mesh/CascadedShadow.glslh, Common/CloudShadowReceiver.glslh,
// Mesh/LocalLightShadow.glslh), so a card cannot be lit by a different formula than the surface it holds.
//
// Two targets, both R11G11B10F atlas layers: DirectLighting = the irradiance (sum of radiance x cos x shadow, UE
// stores the same), FinalLighting = albedo / PI x direct + albedo x indirect + emissive (UE CombineLumenSceneLighting,
// Diffuse_Lambert), indirect = the page's IndirectLighting layer as SurfaceCacheIndirectLighting.shader last wrote it
// (already / PI); zero on a page captured this frame (its layer holds a stranger's texels until that pass runs).
//
// One instance per page to relight: six vertices cover the page's rectangle of the atlas; the fragment works out
// its card texel from the page's card rectangle. A texel the capture saw no surface in (opacity 0) is unlit.
//
// OFF THE VIEW'S SHADOW MAPS (GI-SURFACE-CACHE-5, GI-CARD-LOOKUP; UE LumenSceneDirectLightingShadowMask.usf, the
// traced shadow ray of TraceDistanceFieldShadows / bLumenUseGlobalSDF or the hardware shadow trace): a texel no
// cascade covers (behind the camera, past the shadow distance) and a texel of a shadow-casting point/spot light the
// local shadow budget refused a view get their shadow from a visibility ray of the ACTIVE GI TRACER
// (GITraceVisibility, Common/GITracer.glslh: the global distance field march or the ray query - the program is
// compiled per tracer, GlobalIllumination::TracerVariant) instead of "fully lit".
//
// THE LIGHTS OF A TEXEL (UE LumenSceneDirectLightingCulling): the world light grid's list for the texel's cell
// (Mesh/LightGrid.glslh LightGridWorldLightsAt - the radiance cache clipmap cells around the camera, built by
// "LightGridWorld" from the same point/spot buffers), not every light of the view.
Shader "SurfaceCacheDirectLighting"
{
    Vertex
    {
        #include <Common/SurfaceCache.glslh>
        #include <Common/ScreenUV.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        // Indices into SurfaceCardPageBuffer: the pages relit this frame (SceneFrame::LightingPages).
        ReadBuffer(1) SurfaceLightingPages
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
            v_Captured    = page.card.z; // 1: captured this frame (SurfaceCacheScene::Update)
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
        ReadBuffer(2) SurfaceCardBuffer
        {
            SurfaceCard surfaceCards[];
        };
        Uniform(3) sampler2D u_AtlasDepth;
        Uniform(4) sampler2D u_AtlasAlbedo;
        Uniform(5) sampler2D u_AtlasOpacity;
        Uniform(7) sampler2D u_AtlasNormal;
        Uniform(8) sampler2D u_AtlasEmissive;

        #include <Mesh/PointLight.glslh> // binding 6  (PointLightsUB) + LightSources + BRDF
        #include <Mesh/Spotlight.glslh>  // binding 16 (SpotLightsUB)

        // The sun's cascades: the composite's maps and the composite's ShadowUB values (same packer on the C++
        // side, SurfaceCacheRenderer::DirectLightingInputs), as a storage block of the same members.
        Uniform(9) sampler2D u_ShadowMap0;
        Uniform(10) sampler2D u_ShadowMap1;
        Uniform(11) sampler2D u_ShadowMap2;
        Uniform(12) sampler2D u_ShadowMap3;
        ReadBuffer(13) SurfaceCacheShadowUB
        {
            mat4 u_LightViewProj[4];
            vec4 u_ShadowParams;      // x = 0, y = enabled (>0.5), z = debug mode, w = cascade count
            vec4 u_DebugParams;       // unused here
            vec4 u_CascadeTexelWorld; // per-cascade world size of one shadow-map texel
        };
        #include <Mesh/CascadedShadow.glslh>

        Uniform(14) sampler2D u_CloudShadowMap;
        ReadBuffer(15) SurfaceCacheCloudShadowUB
        {
            mat4 u_CloudShadowWorldToMap;
            vec4 u_CloudShadowParams; // Graphic::CloudShadowPackUniforms
        };
        #include <Common/CloudShadowReceiver.glslh>

        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(17) sampler2D u_LocalShadowAtlas;
        ReadBuffer(18) LocalShadowViewsUB
        {
            LocalShadowView localShadowViews[];
        };
        #include <Mesh/LocalLightShadow.glslh>

        // The world light grid (SceneRenderer::AddFrameLightGridWorld): its parameters, then its cell lists.
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(19) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(20) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>

        // The indirect light the page last received (SurfaceCacheIndirectLighting.shader).
        Uniform(26) sampler2D u_AtlasIndirectLighting;

        // The active GI tracer's reads (GlobalIllumination::DeclareTracerReads), bindings 21..25.
        #define GI_TRACER_BINDING 21
        #include <Common/GITracer.glslh>

        PushConstant SurfaceDirectLightingPush
        {
            vec4  u_LightDir;   // xyz = the direction the sun's light travels
            vec4  u_LightColor; // rgb = colour, a = intensity
            uvec4 u_Flags;      // x = 1: the indirect lighting runs this frame (the pages keep their IndirectLighting)
        };

        // The shadow of a light in direction @p L (unit, toward the light) at most @p maxT away, by a visibility ray
        // of the active tracer: 0 when the ray meets a surface, 1 otherwise. The ray leaves the texel by the
        // tracer's own offset (GIRayBias: the software walk's voxel, UE's surface bias for the global SDF shadow ray)
        // and stops that far short of a local light (its own fixture).
        float TracedShadow(vec3 worldPos, vec3 N, vec3 L, float maxT)
        {
            const float offset = GIRayBias(worldPos, N, L).normalOffset;
            if (maxT <= 2.0 * offset)
                return 1.0;
            return GITraceVisibility(worldPos + N * offset, L, maxT - 2.0 * offset);
        }

        // A point/spot light's shadow at the texel: its local shadow view when it has one, the distance field when
        // it casts but the view budget gave it none.
        float LocalLightShadowOrTrace(int shadowView, int shadowCaster, float viewShadow, vec3 lightPos, vec3 worldPos,
                                      vec3 N)
        {
            if (shadowView >= 0)
                return viewShadow;
            if (shadowCaster == 0)
                return 1.0;
            const vec3  toLight = lightPos - worldPos;
            const float dist    = length(toLight);
            return dist > 0.0 ? TracedShadow(worldPos, N, toLight / dist, dist) : 1.0;
        }

        layout(location = 0) in flat uint v_CardIndex;
        layout(location = 1) in flat vec4 v_CardUVRect;
        layout(location = 2) in flat vec4 v_AtlasUVRect;
        layout(location = 3) in flat uint v_Captured;
        Out(0) vec4 oDirect;
        Out(1) vec4 oFinal;

        // [-1, 1]² octahedral -> unit vector (UE OctahedronToUnitVector).
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
                oDirect = vec4(0.0);
                oFinal  = vec4(0.0);
                return;
            }
            const SurfaceCard card = surfaceCards[v_CardIndex];
            const vec2  res        = vec2(uvec2(1u << (card.info.x & 0xFFu), 1u << ((card.info.x >> 8u) & 0xFFu)));
            const vec2  atlasSize  = vec2(textureSize(u_AtlasOpacity, 0));
            const ivec2 pageOrigin = ivec2(v_AtlasUVRect.xy * atlasSize + 0.5);
            const vec2  cardTexel  = floor(v_CardUVRect.xy * res + 0.5) + vec2(atlasTexel - pageOrigin) + 0.5;
            // Card UV: u along the card's X, v down its Y (the capture's rows run top-down) - as the copy and the
            // Lumen Scene view read it.
            const vec2 uv     = cardTexel / res;
            const vec2 local  = vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y) * card.localExtent.xy;
            const vec3 x      = card.worldToLocal[0].xyz;
            const vec3 y      = card.worldToLocal[1].xyz;
            const vec3 z      = card.worldToLocal[2].xyz;
            const vec3 origin = vec3(card.worldToLocal[0].w, card.worldToLocal[1].w, card.worldToLocal[2].w);
            const float depth = texelFetch(u_AtlasDepth, atlasTexel, 0).r;
            const vec3 worldPos = origin + x * local.x + y * local.y +
                                  z * (card.localExtent.z - depth * 2.0 * card.localExtent.z);
            const vec3 N = OctahedronToUnitVector(texelFetch(u_AtlasNormal, atlasTexel, 0).rg * 2.0 - 1.0);

            vec3 irradiance = vec3(0.0);
            // The sun: always (a scene without one carries intensity 0), its cascades times the cloud layer.
            {
                const vec3  L     = normalize(-u_LightDir.xyz);
                const float NdotL = max(dot(N, L), 0.0);
                if (NdotL > 0.0)
                {
                    int   cascade = -1;
                    float shadow  = ShadowFactor(worldPos, N, L, cascade);
                    // Shadows on (u_ShadowParams.y) but no cascade holds the texel: the distance field answers.
                    if (cascade < 0 && u_ShadowParams.y > 0.5)
                        shadow = TracedShadow(worldPos, N, L, 1.0e7);
                    irradiance += u_LightColor.rgb * u_LightColor.a * NdotL * shadow * CloudShadowFactor(worldPos);
                }
            }
            // The point and spot lights the world light grid lists for the texel's cell (the card may be off-screen:
            // the froxel grid does not cover it), each with its shadow.
            const LightGridWorldRange range = LightGridWorldLightsAt(lightGrid, worldPos);
            for (uint k = 0u; k < range.count; ++k)
            {
                const uint i = LightGridWorldLight(range, k);
                if (i < lightGrid.counts.x)
                {
                    const PointLight source = pointLights[i];
                    DesertLight      light  = DesertPointLightAt(source, worldPos);
                    if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) > 0.0)
                        light.Shadow = LocalLightShadowOrTrace(
                            source.shadowView, source.shadowCaster,
                            source.shadowView >= 0 ? PointLightShadow(source, worldPos, N) : 1.0, source.position,
                            worldPos, N);
                    irradiance += light.Radiance * light.Shadow * max(dot(N, light.L), 0.0);
                }
                else
                {
                    const SpotLight source = spotLights[i - lightGrid.counts.x];
                    DesertLight     light  = DesertSpotLightAt(source, worldPos);
                    if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) > 0.0)
                        light.Shadow = LocalLightShadowOrTrace(
                            source.shadowView, source.shadowCaster,
                            source.shadowView >= 0 ? SpotLightShadow(source, worldPos, N) : 1.0, source.position,
                            worldPos, N);
                    irradiance += light.Radiance * light.Shadow * max(dot(N, light.L), 0.0);
                }
            }

            const vec3 albedo   = texelFetch(u_AtlasAlbedo, atlasTexel, 0).rgb;
            const vec3 emissive = texelFetch(u_AtlasEmissive, atlasTexel, 0).rgb;
            oDirect = vec4(irradiance, 0.0);
            const vec3 indirect =
                 (v_Captured != 0u || u_Flags.x == 0u) ? vec3(0.0) : texelFetch(u_AtlasIndirectLighting, atlasTexel, 0).rgb;
            oFinal  = vec4(albedo / PI * irradiance + albedo * indirect + emissive, 0.0);
        }
    }
}

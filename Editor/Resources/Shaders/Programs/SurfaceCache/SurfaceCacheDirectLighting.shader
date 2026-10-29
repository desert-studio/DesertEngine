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
// Two targets, both R11G11B10F atlas layers: DirectLighting = the diffuse radiance the texel leaves, albedo / PI x
// the front irradiance (sum of radiance x cos x shadow on +N) + subsurface / PI x the back irradiance (the same on
// -N, see below); FinalLighting = direct + indirect + emissive (UE CombineLumenSceneLighting, Diffuse_Lambert),
// indirect = the page's IndirectLighting layer as SurfaceCacheIndirectLighting.shader last wrote it; zero on a page
// captured this frame (its layer holds a stranger's texels until that pass runs).
//
// THE LIGHT THROUGH A LEAF (FOLIAGE-LOOK-5): a texel of a subsurface model (the Subsurface layer, the capture's
// DesertSubsurfaceColor - TwoSidedFoliage's SubsurfaceColor) also takes every light on its far side, -N, times its
// subsurface colour: the sun with the cascades' TransmissionShadow (the thickness of what stands between the leaf
// and the sun, as the deferred composite gives TwoSidedFoliage's transmission), the local lights with their shadow
// from the far side. Without it a ray that met a leaf's underside under a lit canopy read black: the radiance cache
// above a dense crown held only dark undersides and the crown went dark (FOLIAGE-LOOK-4).
// WHERE THIS DIFFERS FROM UE (for the owner): UE's card capture folds SubsurfaceColor into the card's diffuse
// albedo (LumenCardPixelShader.usf) and lights it from +N only, so a leaf's underside under the sun stays as dark as
// an opaque one in the cache - the very defect here. The cache keeps the colour apart and lights the far side; a
// card texel is view-independent, so the far side is Lambert (the deferred's forward-scatter lobe needs a viewer).
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
//
// THE EMISSIVE SURFACES AS LIGHTS (GI-EMISSIVE-LIGHTS; NVIDIA RTXDI's emissive triangles in one light list, the
// path tracer's next event estimation): under either GI tracer a texel also receives the scene's emissive
// triangles (Common/EmissiveLights.glslh, the ONE list RayTracing::EmissiveLightsFrame the path tracer samples):
// kEmissiveSamplesPerTexel triangles picked by power, a point on each by area, the segment's radiance estimate
// over the solid-angle density, shadowed by the tracer's visibility ray. A lamp's glowing shade lights the wall
// beside it in the cache, so every GI ray that meets that wall carries the lamp's bounce - not only the rays that
// happen to meet the small shade itself (UE Lumen's noise of small emitters: its cards take the emissive only as
// the emitter's own radiance).
// The radiosity (SurfaceCacheRadiosityTrace.shader) leaves the emission of the cards its rays meet out under the
// same tracer (GI_HIT_EXCLUDES_EMISSION): the emitter -> texel path is counted here, once.
// WHERE THIS DIFFERS FROM UE (for the owner): UE has no emissive light sampling in Lumen; MegaLights' emissive
// support is the path tracer's. The software tracer has no list (it is built from the TLAS segment records) -
// GI-EMISSIVE-LIGHTS-SW.
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
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass and numbered by the program build (SharedSamplerBinding.hpp); a
        // same-name macro after each texture keeps the sampled form. The repeat state is the software tracer's too
        // (Common/GITracer.glslh): declared once, under its guard.
        #ifndef DESERT_SHARED_LINEAR_REPEAT_SAMPLER
        #define DESERT_SHARED_LINEAR_REPEAT_SAMPLER
        Uniform sampler LinearRepeatSampler;
        #endif
        Uniform sampler PointClampSampler;
        Uniform(3) texture2D AtlasDepthTexture;
        #define AtlasDepthTexture sampler2D(AtlasDepthTexture, PointClampSampler)
        Uniform(4) texture2D AtlasAlbedoTexture;
        #define AtlasAlbedoTexture sampler2D(AtlasAlbedoTexture, PointClampSampler)
        Uniform(5) texture2D AtlasOpacityTexture;
        #define AtlasOpacityTexture sampler2D(AtlasOpacityTexture, PointClampSampler)
        Uniform(7) texture2D AtlasNormalTexture;
        #define AtlasNormalTexture sampler2D(AtlasNormalTexture, PointClampSampler)
        Uniform(8) texture2D AtlasEmissiveTexture;
        #define AtlasEmissiveTexture sampler2D(AtlasEmissiveTexture, PointClampSampler)

        #include <Mesh/PointLight.glslh> // binding 6  (PointLightsUB) + LightSources + BRDF
        #include <Mesh/Spotlight.glslh>  // binding 16 (SpotLightsUB)

        // The sun's cascades: the composite's maps and the composite's ShadowUB values (same packer on the C++
        // side, SurfaceCacheRenderer::DirectLightingInputs), as a storage block of the same members.
        Uniform(9) texture2DArray ShadowCascadesTexture; // one layer per cascade (10..12 free)
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)
        ReadBuffer(13) SurfaceCacheShadowUB
        {
            mat4 LightViewProjection[8];  // [light * 4 + cascade] - Mesh/ShadowUB.glslh's members, same packer
            vec4 DirectionalShadow[2];    // per slot: x = tan(source angle / 2), y = casts, z = cascades, w = first layer
            vec4 CascadeTexelWorld[2];
            vec4 CascadeDepthRange[2];
            vec4 DebugParams;             // unused here
            // The view's contact shadow values (SceneResources::PackContactShadow). Unread: a card texel has no
            // screen depth to march (SceneRenderer::AddFrameSurfaceCache packs 0 steps), and this pass includes no
            // Common/ContactShadow.glslh, so Mesh/LocalLightShadow.glslh offers it the shadow-map terms only.
            vec4 ContactShadowView;
            vec4 SunContactShadow;
            vec4 ShadowFilter;      // x = PCSS blocker search taps, y = filter taps (sun PCSS + local lights PCF)
        };
        #include <Mesh/CascadedShadow.glslh>

        Uniform(14) texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearRepeatSampler)
        ReadBuffer(15) SurfaceCacheCloudShadowUB
        {
            mat4 CloudShadowWorldToMap;
            vec4 CloudShadowParams; // Graphic::CloudShadowPackUniforms
        };
        #include <Common/CloudShadowReceiver.glslh>

        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(17) texture2D LocalShadowAtlasTexture;
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
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
        Uniform(27) texture2D AtlasIndirectLightingTexture;
        #define AtlasIndirectLightingTexture sampler2D(AtlasIndirectLightingTexture, PointClampSampler)
        // The light the texel lets through (rgb) and its subsurface opacity (a); 0 = opaque (SurfaceCacheCardCopy).
        Uniform(28) texture2D AtlasSubsurfaceTexture;
        #define AtlasSubsurfaceTexture sampler2D(AtlasSubsurfaceTexture, PointClampSampler)

        #include <Common/SubsurfaceDensity.glslh> // ContactShadowSubsurfaceDensity

        // The active GI tracer's reads (GlobalIllumination::DeclareTracerReads), bindings 29..39.
        #define GI_TRACER_BINDING 29
        #include <Common/GITracer.glslh>
        #include <Common/LightSourceSampling.glslh> // LightSourceSphereAxisReach: a traced local shadow ends on the emitter

        PushConstant SurfaceDirectLightingPush
        {
            vec4  LightDirection;   // xyz = the direction the sun's light travels
            vec4  LightColor; // rgb = colour, a = intensity
            vec4  Light1Direction;  // atmosphere light 1 (the moon): xyz = the direction its light travels
            vec4  Light1Color;      // rgb = colour, a = intensity (after the atmosphere's transmittance); 0: none
            uvec4 LightingFlags;      // x = 1: the indirect lighting runs this frame (the pages keep their IndirectLighting); y = frame index (the emissive samples' seed)
            uvec4 EmissiveCounts;        // the static part: x = triangles, y = power tree leaves, z = its levels, w = TLAS instances of the hit lookup (0 without a TLAS)
            uvec4 EmissiveDynamicCounts; // the dynamic part: x = triangles, y = power tree leaves, z = its levels
        };

        // The scene's emissive triangles (RayTracing::EmissiveLightsFrame), bindings 40..46 after the tracer's.
        #include <Common/EmissiveLights.glslh>
        layout(std430, binding = 40) readonly buffer EmissiveSegmentsUB
        {
            EmissiveSegment emissiveSegments[];
        };
        layout(std430, binding = 41) readonly buffer EmissiveLookupUB
        {
            uint emissiveLookup[];
        };
        layout(std430, binding = 42) readonly buffer EmissiveTrianglesUB
        {
            EmissiveTriangle emissiveTriangles[];
        };
        layout(std430, binding = 43) readonly buffer EmissivePowerUB
        {
            float emissivePower[];
        };
        layout(std430, binding = 44) readonly buffer EmissiveDynamicSegmentsUB
        {
            EmissiveSegment emissiveDynamicSegments[];
        };
        layout(std430, binding = 45) readonly buffer EmissiveDynamicTrianglesUB
        {
            EmissiveTriangle emissiveDynamicTriangles[];
        };
        layout(std430, binding = 46) readonly buffer EmissiveDynamicPowerUB
        {
            float emissiveDynamicPower[];
        };
        #include <Common/EmissiveLightsSampling.glslh>

        // Emissive triangles sampled per texel and relight (the radiosity's temporal accumulation and the GI
        // gathers that read many texels average them).
        const uint kEmissiveSamplesPerTexel = 8u;

        // A per-texel, per-frame random stream (PCG hash, as the radiosity's strata jitter).
        uint EmissiveHash(uvec3 v)
        {
            v = v * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            return v.x ^ v.y ^ v.z;
        }
        float EmissiveRandom(inout uint state)
        {
            state = state * 747796405u + 2891336453u;
            const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
        }

        // The shadow of a light in direction @p L (unit, toward the light) at most @p maxT away, by a visibility ray
        // of the active tracer: 0 when the ray meets a surface, 1 otherwise. The ray leaves the texel by the
        // tracer's own offsets (GIRayBias: the near field's few cm, the global field's voxel before it counts - UE's
        // mesh-SDF and global-SDF surface biases for the shadow ray) and stops the near offset short of a local
        // light twice over (its own fixture).
        float TracedShadow(vec3 worldPos, vec3 N, vec3 L, float maxT)
        {
            const GITraceBias bias = GIRayBias(worldPos, N, L);
            const float offset = bias.nearOffset;
            if (maxT <= 2.0 * offset)
                return 1.0;
            return GITraceVisibility(worldPos + N * offset, L, GIFarFieldTMin(bias, N, L), maxT - 2.0 * offset);
        }

        // ONE SHADOWED DIRECTIONAL LIGHT at the texel, by its slot @p light of the cascades (kShadowLightSun,
        // kShadowLightAtmosphere1): the light travelling along @p travel with @p radiance, occluded by that slot's
        // cascades - and, where the slot casts but no cascade holds the texel, by the tracer's ray - times the cloud
        // layer when @p cloudShadowed (the cloud shadow map is fitted to the SUN's direction, as in the deferred
        // composite). Adds the front irradiance to @p irradiance and, on a subsurface texel, the far side's to @p back.
        void AddDirectionalLight(int light, vec3 travel, vec3 radiance, bool cloudShadowed, vec3 worldPos, vec3 N,
                                 bool through, float subsurfaceOpacity, inout vec3 irradiance, inout vec3 back)
        {
            const vec3  L     = normalize(-travel);
            const float NdotL = dot(N, L);
            const bool  casts = DirectionalShadow[light].y > 0.5;
            if (NdotL > 0.0)
            {
                int   cascade = -1;
                float shadow  = ShadowFactor(light, worldPos, N, L, cascade);
                // Shadows on but no cascade holds the texel: the tracer answers.
                if (cascade < 0 && casts)
                    shadow = TracedShadow(worldPos, N, L, 1.0e7);
                irradiance += radiance * NdotL * shadow * (cloudShadowed ? CloudShadowFactor(worldPos) : 1.0);
            }
            else if (through && NdotL < 0.0)
            {
                // The deferred composite's TransmissionShadow (Mesh/CascadedShadow.glslh): the cascades' thickness
                // transmittance at the receiver's own depth; off the cascades, the tracer's ray from the far side.
                float shadow = casts && chooseCascade(light, worldPos) < 0
                                   ? TracedShadow(worldPos, -N, L, 1.0e7)
                                   : TransmissionShadowFactor(light, worldPos, ContactShadowSubsurfaceDensity(subsurfaceOpacity));
                back += radiance * -NdotL * shadow * (cloudShadowed ? CloudShadowFactor(worldPos) : 1.0);
            }
        }

        // A point/spot light's shadow at the texel: its local shadow view when it has one, the distance field when
        // it casts but the view budget gave it none - a ray that ends on the light's emitter sphere of @p sourceRadius
        // (the shadow view's near plane does the same, Graphic::LocalShadowNear).
        float LocalLightShadowOrTrace(int shadowView, int shadowCaster, float viewShadow, vec3 lightPos,
                                      float sourceRadius, vec3 worldPos, vec3 N)
        {
            if (shadowView >= 0)
                return viewShadow;
            if (shadowCaster == 0)
                return 1.0;
            const vec3  toLight = lightPos - worldPos;
            const float dist    = length(toLight);
            return dist > 0.0 ? TracedShadow(worldPos, N, toLight / dist, LightSourceSphereAxisReach(dist, sourceRadius))
                              : 1.0;
        }

        // The emissive triangles' irradiance at the texel (front, +N, into @p irradiance; the far side, -N, into
        // @p back on a subsurface texel): an unbiased estimate of kEmissiveSamplesPerTexel samples, each a triangle by
        // power and a point on it by area (the path tracer's PathTracingSampleEmissive), radiance x |cos| / its
        // solid-angle density, shadowed by the tracer's visibility ray. The ray stops short of the sampled point by
        // a margin (the emitter's own surface is not its occluder); emission is two-sided (EmissiveLightsNormal).
        void AddEmissiveLights(vec3 worldPos, vec3 N, bool through, uvec2 seed, inout vec3 irradiance, inout vec3 back)
        {
            if (EmissiveLightsTriangleCount(EmissiveCounts, EmissiveDynamicCounts) == 0u ||
                EmissiveLightsTotal(EmissiveCounts, EmissiveDynamicCounts) <= 0.0)
                return;
            uint        rng   = EmissiveHash(uvec3(seed, LightingFlags.y));
            const float scale = 1.0 / float(kEmissiveSamplesPerTexel);
            for (uint s = 0u; s < kEmissiveSamplesPerTexel; ++s)
            {
                const float pick     = EmissiveRandom(rng);
                const vec2  u        = vec2(EmissiveRandom(rng), EmissiveRandom(rng));
                const uint  triangle = EmissiveLightsPick(pick, EmissiveCounts, EmissiveDynamicCounts);
                if (triangle >= EmissiveLightsTriangleCount(EmissiveCounts, EmissiveDynamicCounts))
                    continue;
                const EmissiveTriangle t       = EmissiveLightsTriangle(triangle, EmissiveCounts);
                const vec3             toPoint = EmissiveLightsSamplePoint(t, u) - worldPos;
                const float            dist    = length(toPoint);
                if (dist <= 0.0)
                    continue;
                const vec3  L        = toPoint / dist;
                const float NdotL    = dot(N, L);
                const bool  front    = NdotL > 0.0;
                if (!front && !(through && NdotL < 0.0))
                    continue;
                const float lightPdf = EmissiveLightsSolidAnglePdf(
                    t, EmissiveLightsPickPdf(triangle, EmissiveCounts, EmissiveDynamicCounts), dist,
                    dot(EmissiveLightsNormal(t), L));
                if (!(lightPdf > 0.0))
                    continue;
                const uint            row     = floatBitsToUint(t.Edge2Segment.w);
                const EmissiveSegment segment = triangle < EmissiveCounts.x ? emissiveSegments[row]
                                                                            : emissiveDynamicSegments[row];
                const vec3 radiance = segment.Radiance.rgb;
                if (max(radiance.r, max(radiance.g, radiance.b)) <= 0.0)
                    continue;
                // Short of the sampled point by 1 % (the emitter's own triangles do not shadow it).
                const vec3  side       = front ? N : -N;
                const float visibility = TracedShadow(worldPos, side, L, dist * 0.99);
                if (visibility <= 0.0)
                    continue;
                const vec3 contribution = radiance * abs(NdotL) * visibility * (scale / lightPdf);
                if (front)
                    irradiance += contribution;
                else
                    back += contribution;
            }
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
            if (texelFetch(AtlasOpacityTexture, atlasTexel, 0).r < 0.5)
            {
                oDirect = vec4(0.0);
                oFinal  = vec4(0.0);
                return;
            }
            const SurfaceCard card = surfaceCards[v_CardIndex];
            const vec2  res        = vec2(uvec2(1u << (card.info.x & 0xFFu), 1u << ((card.info.x >> 8u) & 0xFFu)));
            const vec2  atlasSize  = vec2(textureSize(AtlasOpacityTexture, 0));
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
            const float depth = texelFetch(AtlasDepthTexture, atlasTexel, 0).r;
            const vec3 worldPos = origin + x * local.x + y * local.y +
                                  z * (card.localExtent.z - depth * 2.0 * card.localExtent.z);
            const vec3 N = OctahedronToUnitVector(texelFetch(AtlasNormalTexture, atlasTexel, 0).rg * 2.0 - 1.0);

            const vec4 subsurface  = texelFetch(AtlasSubsurfaceTexture, atlasTexel, 0);
            const bool through     = max(subsurface.r, max(subsurface.g, subsurface.b)) > 0.0;
            vec3       irradiance  = vec3(0.0);
            vec3       back        = vec3(0.0); // the irradiance on the far side, -N (a subsurface texel only)
            // The sun: always (a scene without one carries intensity 0), its cascades times the cloud layer.
            AddDirectionalLight(kShadowLightSun, LightDirection.xyz, LightColor.rgb * LightColor.a, true, worldPos, N,
                                through, subsurface.a, irradiance, back);
            // Atmosphere light 1 (the moon), as the deferred composite lights it: its own cascades (slot
            // kShadowLightAtmosphere1), no cloud shadow; absent (a == 0) adds nothing.
            if (Light1Color.a > 0.0)
                AddDirectionalLight(kShadowLightAtmosphere1, Light1Direction.xyz, Light1Color.rgb * Light1Color.a,
                                    false, worldPos, N, through, subsurface.a, irradiance, back);
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
                            source.sourceRadius, worldPos, N);
                    irradiance += light.Radiance * light.Shadow * max(dot(N, light.L), 0.0);
                    if (through && dot(N, light.L) < 0.0)
                        back += light.Radiance * -dot(N, light.L) *
                                LocalLightShadowOrTrace(
                                     source.shadowView, source.shadowCaster,
                                     source.shadowView >= 0 ? PointLightShadow(source, worldPos, -N) : 1.0,
                                     source.position, source.sourceRadius, worldPos, -N);
                }
                else
                {
                    const SpotLight source = spotLights[i - lightGrid.counts.x];
                    DesertLight     light  = DesertSpotLightAt(source, worldPos);
                    if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) > 0.0)
                        light.Shadow = LocalLightShadowOrTrace(
                            source.shadowView, source.shadowCaster,
                            source.shadowView >= 0 ? SpotLightShadow(source, worldPos, N) : 1.0, source.position,
                            source.sourceRadius, worldPos, N);
                    irradiance += light.Radiance * light.Shadow * max(dot(N, light.L), 0.0);
                    if (through && dot(N, light.L) < 0.0)
                        back += light.Radiance * -dot(N, light.L) *
                                LocalLightShadowOrTrace(
                                     source.shadowView, source.shadowCaster,
                                     source.shadowView >= 0 ? SpotLightShadow(source, worldPos, -N) : 1.0,
                                     source.position, source.sourceRadius, worldPos, -N);
                }
            }

            // The emissive surfaces of the scene (THE EMISSIVE SURFACES AS LIGHTS above).
            AddEmissiveLights(worldPos, N, through, uvec2(atlasTexel), irradiance, back);

            const vec3 albedo   = texelFetch(AtlasAlbedoTexture, atlasTexel, 0).rgb;
            const vec3 emissive = texelFetch(AtlasEmissiveTexture, atlasTexel, 0).rgb;
            const vec3 direct   = albedo / PI * irradiance + subsurface.rgb / PI * back;
            oDirect = vec4(direct, 0.0);
            const vec3 indirect =
                 (v_Captured != 0u || LightingFlags.x == 0u) ? vec3(0.0) : texelFetch(AtlasIndirectLightingTexture, atlasTexel, 0).rgb;
            oFinal  = vec4(direct + indirect + emissive, 0.0);
        }
    }
}

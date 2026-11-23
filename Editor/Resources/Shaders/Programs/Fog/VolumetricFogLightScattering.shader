// DesertAsset {"Kind":"Shader","Guid":"5c1e7a90d34b4f2e8a61b0c97f2d4e31","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VolumetricFogLightScattering"
{
    Compute
    {
        // THE LIT MEDIUM OF ONE FROXEL (UE VolumetricFog.usf LightScatteringCS, reimplemented). One thread per
        // froxel: the height fog's medium at a jittered point inside it, lit by
        //   * the sun, through its shadow cascades - the light shafts;
        //   * every point and spot light whose bounds touch the point, found through the view's light grid
        //     (LIGHT-CLUSTER) and shadowed through the local shadow atlas, or under Shadows.Method VirtualMaps
        //     (the DESERT_LOCAL_SHADOW_VSM permutation, UE: the froxels sample the light's virtual shadow map)
        //     through the light's virtual map - the halos around lamps;
        //   * the sky, as an isotropic ambient (the distant sky light texel the height fog also reads);
        // each through the Henyey-Greenstein phase of the medium and scaled by its light's
        // VolumetricScatteringIntensity. The result is blended with LAST frame's value at the same world point
        // (temporal reprojection, UE r.VolumetricFog.HistoryWeight) - the jitter moves the sample inside the
        // froxel every frame, and the blend integrates those samples into a smooth volume.
        //
        // Writes RGBA16F: .rgb = in-scattered radiance per cm (scattering x lighting + emission), .a = extinction
        // per cm. Programs/Fog/VolumetricFogIntegration.shader integrates it along the view.

        #include <Common/VolumetricFog.glslh>
        #include <Common/HeterogeneousVolume.glslh>
        #include <Mesh/PointLight.glslh>
        #include <Mesh/Spotlight.glslh>
        #include <Mesh/LightGrid.glslh>
        // SKY_DISTANT_LIGHT_SPHERE_TEXEL only; like HeightFog.shader this pass defines neither LUT callback, so
        // the integrator half of SkyScattering.glslh stays out.
        #include <Common/SkyMedium.glslh>
        #include <Common/SkyScattering.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image3D LightScatteringTexture;

        // Last frame's LightScatteringTexture (the renderer's ping-pong pair). Read only when VfGridPixel.w says
        // it is this view's previous frame at this grid size.
        // The pass's shared sampler states (separate textures, RDGShaderParameters.hpp).
        Uniform sampler LinearClampSampler;
        Uniform sampler LinearRepeatSampler;
        Uniform(2) texture3D LightScatteringHistoryTexture;
        #define LightScatteringHistoryTexture sampler3D(LightScatteringHistoryTexture, LinearClampSampler)

        // The sun's cascades: one D32F array, standard Z, read raw with textureGather (Mesh/CascadedShadow.glslh
        // explains the format; that header itself needs gl_FragCoord and cannot be included in compute).
        Uniform(3) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)

        #ifdef DESERT_LOCAL_SHADOW_VSM
        // Shadows.Method VirtualMaps: the point/spot lights' shadows from the view's virtual shadow map, through
        // the reader the opaque composite and the surface cache's cards use (Common/VirtualShadowMap.glslh,
        // VsmLocalVisibility): the atlas is not drawn. A froxel no pass marked reads its light's coarse mip
        // ("VSM: Mark Coarse"), the resolution UE's volumetric fog reads local VSMs at too. Bound by name
        // (VolumetricFogRenderer, kVsm*Block); slots 4/5 are the atlas permutation's, 10/11 free in both.
        #include <Common/VirtualShadowMap.glslh>
        ReadBuffer(4) VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        ReadBuffer(5) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        layout(set = 0, binding = 10, r32ui) restrict readonly uniform uimage2D PhysicalPoolTexture;
        ReadBuffer(11) VsmLocalLightsUB
        {
            VsmLocalLight vsmLocalLights[];
        };
        #define DESERT_VSM_READ
        #include <Common/VirtualShadowMap.glslh>
        #else
        // The point/spot shadow atlas and its views (Mesh/LocalLightShadowLookup.glslh reads them), through the
        // pass's point-clamped state (declared only here: a shared sampler no stage samples is refused).
        Uniform sampler PointClampSampler;
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(4) texture2D LocalShadowAtlasTexture;
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
        ReadBuffer(5) LocalShadowViewsUB { LocalShadowView localShadowViews[]; };
        #endif

        // The sky's distant light texel (Programs/Sky/SkyDistantLight.shader), or the fallback when
        // VfSkyAmbient.w is 0.
        Uniform(7) texture2D DistantSkyLightTexture;
        #define DistantSkyLightTexture sampler2D(DistantSkyLightTexture, LinearClampSampler)

        // THE CLOUD LAYER'S SHADOW ON THE SUN TERM (UE VolumetricCloud "Shadow On Atmosphere", Hillaire 2015's
        // unified volumetrics): the surfaces' receiver text (Common/CloudShadowReceiver.glslh), its pair of
        // values carried in the parameter block. White with Params.y = 0 when no layer casts: never fetched.
        Uniform(12) texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearClampSampler)
        #define CloudShadowWorldToMap VfCloudShadowWorldToMap
        #define CloudShadowParams VfCloudShadowParams
        #include <Common/CloudShadowReceiver.glslh>

        // THE SKY TERM'S VISIBILITY: the view's rain occlusion (Common/RainOcclusion.glslh, the one text the
        // wetted surfaces read) - a froxel under a roof sees no sky, so a barn's interior gets no sky in-scatter.
        // Read with textureGather (raw depths); the includer declares the pair (DESERT_RAIN_OCCLUSION_DECLARED).
        Uniform(13) texture2D RainOcclusionMapTexture;
        #define RainOcclusionSampler LinearClampSampler
        #define RainOcclusionWorldToMap VfRainOcclusionWorldToMap
        #define RainOcclusionParams VfRainOcclusionParams
        #define DESERT_RAIN_OCCLUSION_DECLARED
        #include <Common/RainOcclusion.glslh>

        ReadBuffer(8) LightGridParamsUB { LightGridParams lightGrid; };
        ReadBuffer(9) LightGridCellsUB { uint lightGridCells[]; };
        // The frame's local fog volumes (Graphic::kVolumetricFogLocalVolumesBinding, the pipeline's own buffer
        // like the parameter block); VfLocalFog.x of them are valid.
        ReadBuffer(14) LocalFogVolumesBuffer { LocalFogVolumeGpu localFogVolumes[]; };
        // THE HETEROGENEOUS VOLUMES' SHADOW (smoke between the sun and the fog): their buffer (Graphic::
        // kHeterogeneousVolumesBinding) and this frame's lighting cache, whose .a is the sun's transmittance through
        // the volume from each voxel. VfLocalFog.y of them; 0 = the fallback, never read. The cache is bound by
        // name; its slot only has to be free in THIS program: 16 is Mesh/Spotlight.glslh's SpotLightsUB
        // (ShaderProtocols::kSpotLightsUBBinding), and a texture3D on it made the program refuse to build.
        ReadBuffer(15) HeterogeneousVolumesBuffer { HeterogeneousVolumeGpu heterogeneousVolumes[]; };
        Uniform(18) texture3D HeterogeneousVolumeCacheTexture;
        #define HeterogeneousVolumeCacheTexture sampler3D(HeterogeneousVolumeCacheTexture, LinearClampSampler)

        // A froxel has no fragment and no ShadowUB: its froxel keys the PCF rotation, one tap per froxel.
        #define LOCAL_SHADOW_FILTER_PIXEL vec2(gl_GlobalInvocationID.xy)
        #define LOCAL_SHADOW_FILTER_TAPS 1.0
        #define LOCAL_SHADOW_BLOCKER_SEARCH_TAPS 0.0
        #include <Mesh/LocalLightShadowLookup.glslh>

        LocalSize(4, 4, 4);

        // The sun, the local lights and the sky as every medium receives them (one text with the heterogeneous
        // volumes' lighting cache).
        #include <Common/VolumeLighting.glslh>

        // The sun's transmittance through every heterogeneous volume its ray from p crosses: the cache's .a at the
        // ray's entry into the box (the voxel's own march toward the sun covers the rest of the chord).
        float HeterogeneousVolumeSunTransmittance(vec3 p)
        {
            float t = 1.0;
            for (uint v = 0u; v < VfLocalFog.y; ++v)
            {
                HeterogeneousVolumeGpu volume = heterogeneousVolumes[v];
                vec3 lo  = (volume.WorldToLocal * vec4(p, 1.0)).xyz;
                vec3 ld  = (volume.WorldToLocal * vec4(VfSunDirection.xyz, 0.0)).xyz;
                vec2 hit = HvIntersectUnitBox(lo, ld);
                if (hit.y <= max(hit.x, 0.0))
                    continue;
                vec3 entry = lo + ld * max(hit.x, 0.0);
                t *= textureLod(HeterogeneousVolumeCacheTexture, HvCacheUVW(entry, float(v)), 0.0).a;
            }
            return t;
        }

        void main()
        {
            uvec3 froxel = gl_GlobalInvocationID;
            if (any(greaterThanEqual(froxel, VfGridSize.xyz)))
                return;

            vec3  cell   = vec3(froxel) + VfJitter.xyz;
            float depth  = VolumetricFogDepthFromSlice(cell.z);
            vec3  viewDir;
            vec3  p        = VolumetricFogWorldPosition(cell.xy, depth, viewDir);
            float distance = length(p - VfCamera.xyz);
            float g        = VfCamera.w;

            float extinction = VolumetricFogExtinction(p, distance);
            vec3  scattering = VfAlbedo.rgb * extinction;
            vec3  emissive   = VfEmissive.rgb * extinction;
            // The local fog volumes voxelised into this froxel (UE 5.3 Local Fog Volumes): each adds its own
            // medium, lit below by the same sun, lights and sky. No volume: the loop runs zero times.
            for (uint v = 0u; v < VfLocalFog.x; ++v)
            {
                LocalFogVolumeGpu volume = localFogVolumes[v];
                float             e      = VolumetricFogLocalVolumeExtinction(volume, p);
                extinction += e;
                scattering += volume.Albedo.rgb * e;
                emissive   += volume.Emissive.rgb * e;
            }

            vec3 lighting = vec3(0.0);
            if (extinction > 0.0)
            {
                if (VfSunDirection.w > 0.5)
                    lighting += VfSunRadiance.rgb * SunShadow(p) * CloudShadowFactor(p) *
                                HeterogeneousVolumeSunTransmittance(p) *
                                VolumetricFogPhaseHG(g, dot(VfSunDirection.xyz, viewDir));

                lighting += VolumeLocalLighting(p, viewDir, g);
                lighting += VolumeSkyLighting(p);
            }

            vec4 current = vec4(scattering * lighting + emissive, extinction);

            // Reprojection of the froxel CENTRE (not the jittered point) into last frame's grid.
            if (VfGridPixel.w > 0.5)
            {
                vec3  centreDir;
                vec3  centre    = VolumetricFogWorldPosition(vec2(froxel.xy) + 0.5,
                                                             VolumetricFogDepthFromSlice(float(froxel.z) + 0.5),
                                                             centreDir);
                vec4  prevClip  = VfPrevViewProjection * vec4(centre, 1.0);
                float prevDepth = dot(centre - VfPrevCamera.xyz, VfPrevCameraForward.xyz);
                if (prevClip.w > 0.0 && prevDepth > 0.0)
                {
                    vec2 prevNdc = prevClip.xy / prevClip.w;
                    vec2 prevUV  = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
                    vec3 uvw = vec3(VolumetricFogVolumeUV(prevUV),
                                    VolumetricFogSliceFromDepth(prevDepth) / float(VfGridSize.z));
                    if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0))))
                        current = mix(current, texture(LightScatteringHistoryTexture, uvw), VfJitter.w);
                }
            }

            imageStore(LightScatteringTexture, ivec3(froxel), current);
        }
    }
}

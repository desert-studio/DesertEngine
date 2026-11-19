// DesertAsset {"Kind":"Shader","Guid":"3f0b6c1d2e8a4b7f9c5d1a2e4b6f8c10","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeterogeneousVolumeLightingCache"
{
    Compute
    {
        // THE LIGHTING CACHE OF THE FRAME'S HETEROGENEOUS VOLUMES (UE 5.3+ HeterogeneousVolumes "lighting cache",
        // r.HeterogeneousVolumes.LightingCache 2 = in-scattering, reimplemented). One thread per voxel of a
        // 32^3 grid laid over each on-screen volume's box (one slab per volume along Z of the atlas): the light a
        // point of the medium there receives, so the view march reads one trilinear fetch per step instead of
        // marching a shadow ray per step.
        //   * the sun: its cascades (other geometry), the cloud layer, and THE VOLUME ITSELF - a 12-step march
        //     toward the sun through the box (self-shadowing; .a keeps that transmittance for the froxels, so the
        //     smoke shadows the volumetric fog too);
        //   * the point and spot lights of the view's light grid (Common/VolumeLighting.glslh, the froxels' text);
        //   * the sky, through what covers the voxel (rain occlusion) and a 6-step march up through the volume.
        // Per view, per frame (the phase is the camera's): .rgb = in-scattered radiance per unit scattering
        // coefficient, .a = the sun's transmittance through the volume.

        #include <Common/VolumetricFog.glslh>
        #include <Common/HeterogeneousVolume.glslh>
        #include <Mesh/PointLight.glslh>
        #include <Mesh/Spotlight.glslh>
        #include <Mesh/LightGrid.glslh>
        #include <Common/SkyMedium.glslh>
        #include <Common/SkyScattering.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image3D LightingCacheTexture;

        Uniform sampler LinearClampSampler;
        Uniform sampler LinearRepeatSampler;

        Uniform(3) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)

        // The local lights' shadows: the froxels' two permutations, the same slots and names
        // (VolumetricFogLightScattering.shader says why each one).
        #ifdef DESERT_LOCAL_SHADOW_VSM
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
        Uniform sampler PointClampSampler;
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(4) texture2D LocalShadowAtlasTexture;
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
        ReadBuffer(5) LocalShadowViewsUB { LocalShadowView localShadowViews[]; };
        #endif

        Uniform(7) texture2D DistantSkyLightTexture;
        #define DistantSkyLightTexture sampler2D(DistantSkyLightTexture, LinearClampSampler)

        Uniform(12) texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearClampSampler)
        #define CloudShadowWorldToMap VfCloudShadowWorldToMap
        #define CloudShadowParams VfCloudShadowParams
        #include <Common/CloudShadowReceiver.glslh>

        Uniform(13) texture2D RainOcclusionMapTexture;
        #define RainOcclusionSampler LinearClampSampler
        #define RainOcclusionWorldToMap VfRainOcclusionWorldToMap
        #define RainOcclusionParams VfRainOcclusionParams
        #define DESERT_RAIN_OCCLUSION_DECLARED
        #include <Common/RainOcclusion.glslh>

        ReadBuffer(8) LightGridParamsUB { LightGridParams lightGrid; };
        ReadBuffer(9) LightGridCellsUB { uint lightGridCells[]; };
        // The frame's on-screen volumes (Graphic::kHeterogeneousVolumesBinding, the pipeline's own buffer); VfLocalFog.y
        // of them are valid, volume i in slab i.
        ReadBuffer(15) HeterogeneousVolumesBuffer { HeterogeneousVolumeGpu heterogeneousVolumes[]; };

        // A voxel has no fragment and no ShadowUB: its coordinate keys the PCF rotation, one tap per voxel.
        #define LOCAL_SHADOW_FILTER_PIXEL vec2(gl_GlobalInvocationID.xy)
        #define LOCAL_SHADOW_FILTER_TAPS 1.0
        #include <Mesh/LocalLightShadowLookup.glslh>

        #include <Common/VolumeLighting.glslh>

        LocalSize(4, 4, 4);

        // Optical depth from world point p along the world direction dir through the volume's box, in @p steps.
        float OpticalDepthThrough(HeterogeneousVolumeGpu v, vec3 p, vec3 dir, int steps)
        {
            vec3 localOrigin = (v.WorldToLocal * vec4(p, 1.0)).xyz;
            vec3 localDir    = (v.WorldToLocal * vec4(dir, 0.0)).xyz; // unit box per world cm
            vec2 hit         = HvIntersectUnitBox(localOrigin, localDir);
            float tFar       = max(hit.y, 0.0);
            if (tFar <= 0.0)
                return 0.0;
            float dt  = tFar / float(steps);
            float sum = 0.0;
            for (int i = 0; i < steps; ++i)
            {
                float t = (float(i) + 0.5) * dt;
                sum += HvExtinction(v, localOrigin + localDir * t);
            }
            return sum * dt;
        }

        void main()
        {
            const uint res   = uint(HETEROGENEOUS_VOLUME_CACHE_RES);
            uvec3      voxel = gl_GlobalInvocationID;
            uint       slab  = voxel.z / res;
            if (voxel.x >= res || voxel.y >= res || slab >= VfLocalFog.y)
                return;

            HeterogeneousVolumeGpu v = heterogeneousVolumes[slab];
            vec3 local   = (vec3(voxel.x, voxel.y, voxel.z - slab * res) + 0.5) / float(res) * 2.0 - 1.0;
            vec3 p       = (v.LocalToWorld * vec4(local, 1.0)).xyz;
            vec3 viewDir = normalize(p - VfCamera.xyz);
            float g      = v.Medium.y;

            vec3  lighting = vec3(0.0);
            float sunT     = 1.0;
            if (VfSunDirection.w > 0.5)
            {
                sunT = exp(-OpticalDepthThrough(v, p, VfSunDirection.xyz, 12));
                lighting += VfSunRadiance.rgb * SunShadow(p) * CloudShadowFactor(p) * sunT *
                            VolumetricFogPhaseHG(g, dot(VfSunDirection.xyz, viewDir));
            }
            lighting += VolumeLocalLighting(p, viewDir, g);
            lighting += VolumeSkyLighting(p) * exp(-OpticalDepthThrough(v, p, vec3(0.0, 1.0, 0.0), 6));

            imageStore(LightingCacheTexture, ivec3(voxel), vec4(lighting, sunT));
        }
    }
}

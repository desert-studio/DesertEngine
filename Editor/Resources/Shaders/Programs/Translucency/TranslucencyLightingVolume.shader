// DesertAsset {"Kind":"Shader","Guid":"8e3b6d1f4a2c4e7b9f05c1d2a7e6b384","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TranslucencyLightingVolume"
{
    Compute
    {
        // THE TRANSLUCENCY LIGHTING VOLUME, write side (UE TranslucentLighting.cpp's injection, reimplemented as
        // one gather per cell; Common/TranslucencyLightingVolume.glslh lays the cascades out). One thread per cell of
        // both cascades: the mean incident radiance at the cell's centre from
        //   * the sun, through its shadow cascades and the cloud layer's shadow (the surfaces' very receivers);
        //   * every point and spot light of the view's light grid cell the centre projects to, shadowed through
        //     the local shadow atlas or, under Shadows.Method VirtualMaps (DESERT_LOCAL_SHADOW_VSM), through the
        //     light's virtual shadow map - the readers the volumetric fog's froxels use;
        //   * the sky light: the surfaces' irradiance cube (the view's ambient), through what covers the cell from
        //     the sky (the view's rain occlusion: a barn's interior sees no sky).
        // A directional source of irradiance E adds E / (4 pi); an isotropic radiance L adds L.
        //
        // A cell behind the camera gets no local light: no light grid cell holds it, and no particle of the view
        // reads it (a cell the view sees projects inside the grid; one just outside clamps to its edge cell).

        #define TLV_WRITER
        #include <Common/TranslucencyLightingVolume.glslh>
        #include <Mesh/PointLight.glslh>
        #include <Mesh/Spotlight.glslh>
        #include <Mesh/LightGrid.glslh>

        // Graphic::TranslucencyLightingVolumeGpuPayload (TranslucencyLightingVolume.hpp), member for member; the
        // pipeline's own block (SetStorageBuffer kTranslucencyLightingVolumeParamsBinding), not a graph binding.
        layout(std430, binding = 1) readonly buffer TranslucencyLightingVolumeParams
        {
            mat4 TlvViewProjection;       // world -> clip, this frame's unjittered camera (the light grid's)
            mat4 TlvCascadeViewProj[4];   // the sun's cascades, standard Z (Graphic::CascadeFit::ViewProj)
            vec4 TlvCamera;               // xyz = camera position (cm), w = cells per cascade edge
            vec4 TlvRenderSize;           // xy = render size in pixels (the light grid's), zw unused
            vec4 TlvSunDirection;         // xyz = TOWARD the sun (unit), w = 1 when a sun lights the volume
            vec4 TlvSunRadiance;          // rgb = the sun's irradiance as the surfaces receive it, w = cascades
            vec4 TlvCascadeBias;          // per cascade: depth bias in map units (texel world size / depth range)
            mat4 TlvCloudShadowWorldToMap;   // the cloud layer's shadow map (Graphic::CloudShadowUniforms), the
            vec4 TlvCloudShadowParams;       //   surfaces' pair: y = 0 when no layer casts (never fetched then)
            mat4 TlvRainOcclusionWorldToMap; // the view's rain occlusion (Graphic::RainOcclusionUniforms): what
            vec4 TlvRainOcclusionParams;     //   covers a cell from the sky; y = 0 when the view has no map
        };

        layout(binding = 0, rgba16f) restrict writeonly uniform image3D TranslucencyLightingVolumeOutput;

        Uniform sampler LinearClampSampler;
        Uniform sampler LinearRepeatSampler;

        // The sun's cascades: one D32F array, standard Z, read raw with textureGather (Mesh/CascadedShadow.glslh
        // explains the format; that header needs gl_FragCoord and cannot be included in compute).
        Uniform(2) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)

        #ifdef DESERT_LOCAL_SHADOW_VSM
        // Shadows.Method VirtualMaps: the atlas is not drawn; the local lights' shadows come from the view's
        // virtual shadow map (Common/VirtualShadowMap.glslh, VsmLocalVisibility), bound by name (kVsm*Block).
        #include <Common/VirtualShadowMap.glslh>
        ReadBuffer(3) VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        ReadBuffer(4) VsmPageTable
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
        // The point/spot shadow atlas and its views (Mesh/LocalLightShadowLookup.glslh reads them).
        Uniform sampler PointClampSampler;
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(3) texture2D LocalShadowAtlasTexture;
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
        ReadBuffer(4) LocalShadowViewsUB { LocalShadowView localShadowViews[]; };
        #endif

        // The view's sky light as the surfaces receive it (their EnvironmentIrradianceTexture).
        Uniform(5) textureCube EnvironmentIrradianceTexture;
        #define EnvironmentIrradianceTexture samplerCube(EnvironmentIrradianceTexture, LinearClampSampler)

        // THE CLOUD LAYER'S SHADOW ON THE SUN (Common/CloudShadowReceiver.glslh, the surfaces' receiver text).
        Uniform(12) texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearClampSampler)
        #define CloudShadowWorldToMap TlvCloudShadowWorldToMap
        #define CloudShadowParams TlvCloudShadowParams
        #include <Common/CloudShadowReceiver.glslh>

        // THE SKY TERM'S VISIBILITY: the view's rain occlusion (Common/RainOcclusion.glslh), raw depths gathered.
        Uniform(13) texture2D RainOcclusionMapTexture;
        #define RainOcclusionSampler LinearClampSampler
        #define RainOcclusionWorldToMap TlvRainOcclusionWorldToMap
        #define RainOcclusionParams TlvRainOcclusionParams
        #define DESERT_RAIN_OCCLUSION_DECLARED
        #include <Common/RainOcclusion.glslh>

        ReadBuffer(8) LightGridParamsUB { LightGridParams lightGrid; };
        ReadBuffer(9) LightGridCellsUB { uint lightGridCells[]; };

        // A cell has no fragment and no ShadowUB: its cell keys the PCF rotation, one tap per cell.
        #define LOCAL_SHADOW_FILTER_PIXEL vec2(gl_GlobalInvocationID.xy)
        #define LOCAL_SHADOW_FILTER_TAPS 1.0
        #include <Mesh/LocalLightShadowLookup.glslh>

        LocalSize(4, 4, 4);

        const float TLV_PI = 3.14159265358979;

        // The sun's shadow at p: the first cascade that contains it, one bilinear 2x2 comparison (the volumetric
        // fog's lookup; a cell is a point of medium, not a surface, so no normal offset).
        float SunShadow(vec3 p)
        {
            for (int c = 0; c < int(TlvSunRadiance.w); ++c)
            {
                vec4 lc  = TlvCascadeViewProj[c] * vec4(p, 1.0);
                vec3 ndc = lc.xyz / lc.w;
                vec2 uv  = ndc.xy * 0.5 + 0.5;
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0)
                    continue;
                vec2 size = vec2(textureSize(ShadowCascadesTexture, 0).xy);
                vec4 d    = textureGather(ShadowCascadesTexture, vec3(uv, float(c)), 0);
                vec4 lit  = step(vec4(ndc.z - TlvCascadeBias[c]), d);
                vec2 f    = fract(uv * size - 0.5);
                return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
            }
            return 1.0;
        }

        // Irradiance at p from local light @p index of the grid's numbering (points first, then spots).
        vec3 LocalLight(uint index, vec3 p)
        {
            if (index < lightGrid.counts.x)
            {
                PointLight  source = pointLights[index];
                DesertLight light  = DesertPointLightAt(source, p);
                if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) <= 0.0)
                    return vec3(0.0);
                return light.Radiance * PointLightShadow(source, p, light.L);
            }
            SpotLight   source = spotLights[index - lightGrid.counts.x];
            DesertLight light  = DesertSpotLightAt(source, p);
            if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) <= 0.0)
                return vec3(0.0);
            return light.Radiance * SpotLightShadow(source, p, light.L);
        }

        void main()
        {
            const uint dim  = uint(TlvCamera.w);
            const uvec3 id  = gl_GlobalInvocationID;
            if (id.x >= dim || id.y >= dim || id.z >= dim * TLV_CASCADES)
                return;
            const uint  cascade = id.z / dim;
            const vec3  local   = vec3(id.x, id.y, id.z - cascade * dim) + 0.5;
            const float cell    = TlvCellSize(cascade, float(dim));
            const vec3  p       = TlvCascadeMin(cascade, TlvCamera.xyz, float(dim)) + local * cell;

            // Directional irradiance: each adds E / (4 pi) to the mean incident radiance.
            vec3 irradiance = vec3(0.0);
            if (TlvSunDirection.w > 0.5)
                irradiance += TlvSunRadiance.rgb * SunShadow(p) * CloudShadowFactor(p);

            const vec4 clip = TlvViewProjection * vec4(p, 1.0);
            if (clip.w > 0.0)
            {
                // The light grid cell the centre projects to, clamped to the screen (a cell just outside the
                // frustum is read only by the trilinear filter of a visible one).
                const vec2 ndc   = clip.xy / clip.w;
                const vec2 pixel = clamp(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * TlvRenderSize.xy,
                                         vec2(0.0), TlvRenderSize.xy - 1.0);
                const uint gridCell = LightGridCellAt(lightGrid, pixel, p);
                const uint count    = lightGridCells[gridCell];
                if (LightGridCellOverflowed(lightGrid, count))
                {
                    for (uint i = 0u; i < lightGrid.counts.x + lightGrid.counts.y; ++i)
                        irradiance += LocalLight(i, p);
                }
                else
                {
                    const uint offset = LightGridListOffset(lightGrid, gridCell);
                    for (uint i = 0u; i < count; ++i)
                        irradiance += LocalLight(lightGridCells[offset + i], p);
                }
            }

            // The sky light: the irradiance cube's mean over the sphere is pi x the mean incident radiance; the
            // six axis taps give that mean exactly up to the cube's SH L2 (the cube is a cosine convolution).
            const vec3 skyIrradiance =
                 textureLod(EnvironmentIrradianceTexture, vec3( 1.0, 0.0, 0.0), 0.0).rgb +
                 textureLod(EnvironmentIrradianceTexture, vec3(-1.0, 0.0, 0.0), 0.0).rgb +
                 textureLod(EnvironmentIrradianceTexture, vec3(0.0,  1.0, 0.0), 0.0).rgb +
                 textureLod(EnvironmentIrradianceTexture, vec3(0.0, -1.0, 0.0), 0.0).rgb +
                 textureLod(EnvironmentIrradianceTexture, vec3(0.0, 0.0,  1.0), 0.0).rgb +
                 textureLod(EnvironmentIrradianceTexture, vec3(0.0, 0.0, -1.0), 0.0).rgb;
            const vec3 sky = skyIrradiance / (6.0 * TLV_PI) * (1.0 - DesertRainOcclusionAt(p));

            const vec3 radiance = irradiance / (4.0 * TLV_PI) + sky;
            imageStore(TranslucencyLightingVolumeOutput, ivec3(id), vec4(radiance, 1.0));
        }
    }
}

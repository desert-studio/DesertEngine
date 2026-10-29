// DesertAsset {"Kind":"Shader","Guid":"5c1e7a90d34b4f2e8a61b0c97f2d4e31","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VolumetricFogLightScattering"
{
    Compute
    {
        // THE LIT MEDIUM OF ONE FROXEL (UE VolumetricFog.usf LightScatteringCS, reimplemented). One thread per
        // froxel: the height fog's medium at a jittered point inside it, lit by
        //   * the sun, through its shadow cascades - the light shafts;
        //   * every point and spot light whose bounds touch the point, found through the view's light grid
        //     (LIGHT-CLUSTER) and shadowed through the local shadow atlas - the halos around lamps;
        //   * the sky, as an isotropic ambient (the distant sky light texel the height fog also reads);
        // each through the Henyey-Greenstein phase of the medium and scaled by its light's
        // VolumetricScatteringIntensity. The result is blended with LAST frame's value at the same world point
        // (temporal reprojection, UE r.VolumetricFog.HistoryWeight) - the jitter moves the sample inside the
        // froxel every frame, and the blend integrates those samples into a smooth volume.
        //
        // Writes RGBA16F: .rgb = in-scattered radiance per cm (scattering x lighting + emission), .a = extinction
        // per cm. Programs/Fog/VolumetricFogIntegration.shader integrates it along the view.

        #include <Common/VolumetricFog.glslh>
        #include <Mesh/PointLight.glslh>
        #include <Mesh/Spotlight.glslh>
        #include <Mesh/LocalLightShadowView.glslh>
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
        Uniform sampler PointClampSampler;
        Uniform(2) texture3D LightScatteringHistoryTexture;
        #define LightScatteringHistoryTexture sampler3D(LightScatteringHistoryTexture, LinearClampSampler)

        // The sun's cascades: one D32F array, standard Z, read raw with textureGather (Mesh/CascadedShadow.glslh
        // explains the format; that header itself needs gl_FragCoord and cannot be included in compute).
        Uniform(3) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)

        // The point/spot shadow atlas and its views (Mesh/LocalLightShadowLookup.glslh reads them).
        Uniform(4) texture2D LocalShadowAtlasTexture;
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
        ReadBuffer(5) LocalShadowViewsUB { LocalShadowView localShadowViews[]; };

        // The sky's distant light texel (Programs/Sky/SkyDistantLight.shader), or the fallback when
        // VfSkyAmbient.w is 0.
        Uniform(7) texture2D DistantSkyLightTexture;
        #define DistantSkyLightTexture sampler2D(DistantSkyLightTexture, LinearClampSampler)

        ReadBuffer(8) LightGridParamsUB { LightGridParams lightGrid; };
        ReadBuffer(9) LightGridCellsUB { uint lightGridCells[]; };

        // A froxel has no fragment and no ShadowUB: its froxel keys the PCF rotation, one tap per froxel.
        #define LOCAL_SHADOW_FILTER_PIXEL vec2(gl_GlobalInvocationID.xy)
        #define LOCAL_SHADOW_FILTER_TAPS 1.0
        #include <Mesh/LocalLightShadowLookup.glslh>

        LocalSize(4, 4, 4);

        // The sun's shadow at p: the first cascade that contains it, one bilinear 2x2 comparison (UE's volume
        // shadowing is a single filtered lookup too; the froxel jitter and the history blur it further).
        float SunShadow(vec3 p)
        {
            for (int c = 0; c < int(VfSunRadiance.w); ++c)
            {
                vec4 lc  = VfCascadeViewProj[c] * vec4(p, 1.0);
                vec3 ndc = lc.xyz / lc.w;
                vec2 uv  = ndc.xy * 0.5 + 0.5;
                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0)
                    continue;
                vec2  size  = vec2(textureSize(ShadowCascadesTexture, 0).xy);
                vec4  d     = textureGather(ShadowCascadesTexture, vec3(uv, float(c)), 0);
                vec4  lit   = step(vec4(ndc.z - VfCascadeBias[c]), d);
                vec2  f     = fract(uv * size - 0.5);
                return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
            }
            return 1.0;
        }

        vec3 LocalLight(uint index, vec3 p, vec3 viewDir, float g)
        {
            if (index < lightGrid.counts.x)
            {
                PointLight source = pointLights[index];
                DesertLight light = DesertPointLightAt(source, p);
                if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) <= 0.0)
                    return vec3(0.0);
                return light.Radiance * source.volumetricScatteringIntensity * PointLightShadow(source, p, light.L) *
                       VolumetricFogPhaseHG(g, dot(light.L, viewDir));
            }
            SpotLight source = spotLights[index - lightGrid.counts.x];
            DesertLight light = DesertSpotLightAt(source, p);
            if (max(light.Radiance.r, max(light.Radiance.g, light.Radiance.b)) <= 0.0)
                return vec3(0.0);
            return light.Radiance * source.volumetricScatteringIntensity * SpotLightShadow(source, p, light.L) *
                   VolumetricFogPhaseHG(g, dot(light.L, viewDir));
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

            vec3 lighting = vec3(0.0);
            if (extinction > 0.0)
            {
                if (VfSunDirection.w > 0.5)
                    lighting += VfSunRadiance.rgb * SunShadow(p) *
                                VolumetricFogPhaseHG(g, dot(VfSunDirection.xyz, viewDir));

                // The point's own cell of the light grid: the pixel it projects to and its view depth.
                vec4 clip = VfViewProjection * vec4(p, 1.0);
                vec2 ndc  = clip.xy / max(clip.w, 1e-6);
                vec2 pixel = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * VfGridPixel.yz;
                uint gridCell = LightGridCellAt(lightGrid, pixel, p);
                uint count    = lightGridCells[gridCell];
                if (LightGridCellOverflowed(lightGrid, count))
                {
                    for (uint i = 0u; i < lightGrid.counts.x + lightGrid.counts.y; ++i)
                        lighting += LocalLight(i, p, viewDir, g);
                }
                else
                {
                    uint offset = LightGridListOffset(lightGrid, gridCell);
                    for (uint i = 0u; i < count; ++i)
                        lighting += LocalLight(lightGridCells[offset + i], p, viewDir, g);
                }

                // The sky from every direction at once: the phase integrates to 1 over the sphere.
                vec3 sky = VfSkyAmbient.rgb;
                if (VfSkyAmbient.w > 0.5)
                    sky += VfSkyAmbientScale.rgb *
                           texelFetch(DistantSkyLightTexture, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), 0).rgb;
                lighting += sky;
            }

            vec4 current = vec4(scattering * lighting + VfEmissive.rgb * extinction, extinction);

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

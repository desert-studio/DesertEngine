// DesertAsset {"Kind":"Shader","Guid":"30f0dda90e7646fea408519be79bdf7e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolume"
{
    // "MegaLights: Volume" (UE 5.5 MegaLightsVolume: the translucency volume MegaLights lights). Per FROXEL of the
    // view's frustum (Shadows.MegaLightsVolumeTilePixels square tiles x Shadows.MegaLightsVolumeSlices logarithmic
    // slices, Common/MegaLightsVolume.glslh), at a point jittered inside the froxel every frame:
    //  1) resampled importance sampling over the light-grid cell of that point (every light when the cell
    //     overflowed): Shadows.MegaLightsVolumeSamples lights drawn in proportion to their unshadowed radiance's
    //     luminance there (the point has no surface: no cosine, no BRDF);
    //  2) one visibility ray per drawn light (ray query over the view's TLAS, masked segments alpha-tested) toward a
    //     point of the light's source sphere (SourceRadius: soft shadows over frames), as MegaLights' surface traces;
    //  3) the shadowed estimate projected on L1 spherical harmonics (L0, L1 x y z per colour channel);
    //  4) blended with the previous frame's volume at the point's reprojected froxel (an exponential average of
    //     1 / (Shadows.MegaLightsMaxHistory + 1)), unless the point was outside it or the history is unreadable.
    // The result is next frame's history and what the forward and translucent cells read.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/PointLight.glslh> // binding 6
        #include <Mesh/Spotlight.glslh>  // binding 16

        // MegaLightsVolumeUniforms (MegaLightsVolume.hpp), std140.
        Uniform(0) MegaLightsVolumeViewUB
        {
        	mat4  u_InvViewProjection;  // unjittered, this frame
        	mat4  u_View;               // world -> view, this frame
        	mat4  u_PrevView;           // the view's previous frame
        	mat4  u_PrevViewProjection; // unjittered, the view's previous frame
        	vec4  u_ZParams;            // near (cm), slices / log(far / near), far (cm), froxel edge (render px)
        	vec4  u_PrevZParams;        // the same, of the frame the history was written in
        	uvec4 u_Size;               // xyz = froxels, w = frame index
        	uvec4 u_Counts;             // x = point lights, y = spot lights, z = samples per froxel, w = history readable
        	vec4  u_Params;             // x = history weight (0 = none), y = shadow-ray bias (cm), zw = render extent (px)
        };

        // The view's light grid (Programs/Deferred/LightGridBuild.shader), as the composite reads it.
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(22) LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        ReadBuffer(23) LightGridCellsUB
        {
        	uint lightGridCells[];
        };

        Uniform sampler u_LinearClampSampler;
        Uniform(10) texture3D u_HistoryR;
        #define u_HistoryR sampler3D(u_HistoryR, u_LinearClampSampler)
        Uniform(11) texture3D u_HistoryG;
        #define u_HistoryG sampler3D(u_HistoryG, u_LinearClampSampler)
        Uniform(12) texture3D u_HistoryB;
        #define u_HistoryB sampler3D(u_HistoryB, u_LinearClampSampler)
        layout(binding = 13, rgba16f) writeonly uniform image3D u_OutR; // L0, L1 x y z of red
        layout(binding = 14, rgba16f) writeonly uniform image3D u_OutG;
        layout(binding = 15, rgba16f) writeonly uniform image3D u_OutB;

        #define MEGALIGHTS_VOLUME_MAX_SAMPLES 4
        #define MEGALIGHTS_SH_L0 0.282094792
        #define MEGALIGHTS_SH_L1 0.488602512

        // ---- Random (PCG hash, Jarzynski & Olano 2020), one stream per froxel and frame ----------------------
        uint VolumePcg(uint v)
        {
        	const uint state = v * 747796405u + 2891336453u;
        	const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        	return (word >> 22u) ^ word;
        }

        float VolumeRandom(inout uint state)
        {
        	state = VolumePcg(state);
        	return float(state >> 8u) * (1.0 / 16777216.0);
        }

        // The view depth (cm) at slice coordinate @p slice (texel k's centre is k + 0.5): the reader's inverse
        // (Common/MegaLightsVolume.glslh MegaLightsVolumeUVW).
        float VolumeFroxelDepth(vec4 zParams, float slice)
        {
        	return zParams.x * exp(slice / zParams.y);
        }

        // The world point at render pixel @p pixel (continuous) and view depth @p depth: two points of the pixel's
        // ray, the one at that depth between them (perspective and orthographic alike).
        vec3 VolumeWorldPosition(vec2 pixel, float depth)
        {
        	const vec2  uv     = pixel / u_Params.zw;
        	const vec3  a      = ReconstructWorldPosition(uv, 1.0, u_InvViewProjection);
        	const vec3  b      = ReconstructWorldPosition(uv, 0.5, u_InvViewProjection);
        	const float depthA = -(u_View * vec4(a, 1.0)).z;
        	const float depthB = -(u_View * vec4(b, 1.0)).z;
        	return a + (b - a) * ((depth - depthA) / (depthB - depthA));
        }

        // ---- The light index space (Mesh/LightGrid.glslh: points first, then spots) ---------------------------
        DesertLight VolumeLightAt(int light, vec3 p)
        {
        	const int points = int(u_Counts.x);
        	return light < points ? DesertPointLightAt(pointLights[light], p)
        	                      : DesertSpotLightAt(spotLights[light - points], p);
        }

        float VolumeTarget(int light, vec3 p)
        {
        	return dot(VolumeLightAt(light, p).Radiance, vec3(0.2126, 0.7152, 0.0722));
        }

        // How much of @p light reaches the point @p p: a ray toward a point of its source sphere, uniform in the
        // solid angle the sphere subtends (UE UniformSampleCone), stopped short of the sphere by the bias. A light
        // whose shadowCaster is 0 (CastShadows off) needs no ray.
        float VolumeVisibility(int light, vec3 p, inout uint random)
        {
        	const int  points  = int(u_Counts.x);
        	const bool casts   = (light < points ? pointLights[light].shadowCaster
        	                                     : spotLights[light - points].shadowCaster) != 0;
        	if (!casts)
        		return 1.0;
        	const float bias     = u_Params.y;
        	const vec3  toLight  = (light < points ? pointLights[light].position : spotLights[light - points].position) - p;
        	const float distance = length(toLight);
        	const float radius   = light < points ? pointLights[light].sourceRadius : spotLights[light - points].sourceRadius;
        	if (distance <= max(2.0 * bias, radius))
        		return 1.0; // on or inside the emitter
        	const vec3 axis  = toLight / distance;
        	vec3       dir   = axis;
        	float      reach = distance;
        	if (radius > 0.0)
        	{
        		const float sinMax2 = (radius * radius) / (distance * distance);
        		const float cosMax  = sqrt(max(1.0 - sinMax2, 0.0));
        		const float cosT    = 1.0 - VolumeRandom(random) * (1.0 - cosMax);
        		const float sinT    = sqrt(max(1.0 - cosT * cosT, 0.0));
        		const float phi     = 6.28318530718 * VolumeRandom(random);
        		const vec3  up      = abs(axis.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        		const vec3  t       = normalize(cross(up, axis));
        		const vec3  b       = cross(axis, t);
        		dir                 = normalize(t * (cos(phi) * sinT) + b * (sin(phi) * sinT) + axis * cosT);
        		reach = distance * cosT - sqrt(max(radius * radius - distance * distance * sinT * sinT, 0.0));
        	}
        	// A froxel point has no surface to leave: the ray starts at the point, its first bias skipped.
        	return RayQueryVisibility(p + dir * bias, dir, max(reach - 2.0 * bias, 0.0), RAY_TRACING_MASK_SHADOW_CASTER);
        }

        LocalSize(4, 4, 4);

        void main()
        {
        	const ivec3 froxel = ivec3(gl_GlobalInvocationID);
        	if (any(greaterThanEqual(froxel, ivec3(u_Size.xyz))))
        		return;
        	uint random = VolumePcg(uint(froxel.x) + VolumePcg(uint(froxel.y) + VolumePcg(uint(froxel.z) +
        	                                                                               VolumePcg(u_Size.w))));

        	// The point: jittered inside the froxel, so the frames' average covers it whole.
        	const vec3  jitter = vec3(VolumeRandom(random), VolumeRandom(random), VolumeRandom(random));
        	const vec2  pixel  = (vec2(froxel.xy) + jitter.xy) * u_ZParams.w;
        	const float depth  = VolumeFroxelDepth(u_ZParams, float(froxel.z) + jitter.z);
        	const vec3  p      = VolumeWorldPosition(pixel, depth);

        	// 1) Resampled importance sampling over the point's light-grid cell, one draw per sample.
        	const uint samples = clamp(u_Counts.z, 1u, uint(MEGALIGHTS_VOLUME_MAX_SAMPLES));
        	int   chosen[MEGALIGHTS_VOLUME_MAX_SAMPLES];
        	float chosenTarget[MEGALIGHTS_VOLUME_MAX_SAMPLES];
        	for (uint k = 0u; k < uint(MEGALIGHTS_VOLUME_MAX_SAMPLES); ++k)
        	{
        		chosen[k]       = -1;
        		chosenTarget[k] = 0.0;
        	}
        	float      weightSum  = 0.0;
        	const uint cell       = LightGridCellAt(lightGrid, min(pixel, u_Params.zw - 0.5), p);
        	const uint count      = lightGridCells[cell];
        	const bool overflowed = LightGridCellOverflowed(lightGrid, count);
        	const uint candidates = overflowed ? u_Counts.x + u_Counts.y : count;
        	const uint list       = LightGridListOffset(lightGrid, cell);
        	for (uint j = 0u; j < candidates; ++j)
        	{
        		const int   light  = overflowed ? int(j) : int(lightGridCells[list + j]);
        		const float target = VolumeTarget(light, p);
        		if (target <= 0.0)
        			continue;
        		weightSum += target;
        		for (uint k = 0u; k < samples; ++k)
        			if (VolumeRandom(random) * weightSum < target)
        			{
        				chosen[k]       = light;
        				chosenTarget[k] = target;
        			}
        	}

        	// 2) + 3) Each drawn light traced once; its shadowed radiance, weighted by W / samples, projected on L1.
        	vec4 shR = vec4(0.0);
        	vec4 shG = vec4(0.0);
        	vec4 shB = vec4(0.0);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		if (chosen[k] < 0)
        			continue;
        		const float visibility = VolumeVisibility(chosen[k], p, random);
        		if (visibility <= 0.0)
        			continue;
        		const DesertLight light    = VolumeLightAt(chosen[k], p);
        		const vec3        radiance = light.Radiance * (visibility * weightSum / (chosenTarget[k] * float(samples)));
        		const vec4        basis    = vec4(MEGALIGHTS_SH_L0, MEGALIGHTS_SH_L1 * light.L);
        		shR += radiance.r * basis;
        		shG += radiance.g * basis;
        		shB += radiance.b * basis;
        	}

        	// 4) The history at the point's place in the previous frame's volume.
        	const float weight = u_Params.x;
        	if (u_Counts.w != 0u && weight > 0.0)
        	{
        		const vec4  clip      = u_PrevViewProjection * vec4(p, 1.0);
        		const float prevDepth = -(u_PrevView * vec4(p, 1.0)).z;
        		if (clip.w > 0.0 && prevDepth > 0.0)
        		{
        			const vec2  ndc   = clip.xy / clip.w;
        			const vec2  uv    = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); // ScreenUVToNdc's inverse
        			const vec3  size  = vec3(u_Size.xyz);
        			const float slice = prevDepth <= u_PrevZParams.x ? 0.0 : log(prevDepth / u_PrevZParams.x) * u_PrevZParams.y;
        			const vec3  uvw   = vec3(uv * u_Params.zw / (u_PrevZParams.w * size.xy), slice / size.z);
        			if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0))))
        			{
        				shR = mix(texture(u_HistoryR, uvw), shR, weight);
        				shG = mix(texture(u_HistoryG, uvw), shG, weight);
        				shB = mix(texture(u_HistoryB, uvw), shB, weight);
        			}
        		}
        	}
        	imageStore(u_OutR, froxel, shR);
        	imageStore(u_OutG, froxel, shG);
        	imageStore(u_OutB, froxel, shB);
        }
    }
}

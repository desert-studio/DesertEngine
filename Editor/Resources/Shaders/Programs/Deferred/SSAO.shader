// DesertAsset {"Kind":"Shader","Guid":"c9a95e12430cebced5d5e4c735cb68ed","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSAO"
{
    // Screen-space ambient occlusion as Ground-Truth AO (Jimenez et al. 2016, "Practical Real-Time Strategies for
    // Accurate Indirect Occlusion"), ported from UE 5.8 Engine/Shaders/Private/PostProcessAmbientOcclusion.usf:
    // SearchForLargestAngleDual (828-875), ComputeInnerIntegral (951-976), GetRandomVector (992-1018) and
    // GTAOCombinedPSandCS (1025-1103) - r.AmbientOcclusion.Method 1 with r.GTAO.Combined. Adapted: the depth is
    // reconstructed through the jittered inverse view-projection (Common/ReconstructPosition.glslh) and carried into
    // UE's view space (x right, y up, +z forward) through the camera's View matrix; the G-buffer normal is used
    // (UE r.GTAO.UseNormals 1); a sky texel (reversed-Z depth 0) is skipped by the search instead of being a far
    // plane; the step count is the scalability's AmbientOcclusionSamples (UE GTAO_NUMTAPS by shader quality); the
    // result is the integral itself - no 2/PI storage scale (UE re-multiplies it in the spatial filter, which this
    // pass has none of: the per-frame rotation is resolved by the temporal AA after the composite) and no
    // Intensity/Power artist remap (UE defaults 0.5 / 2 halve the occlusion; this is the visibility it measured).
    //
    // WHY GTAO and not the hemisphere point test this replaced: the point test counted samples behind the depth
    // buffer and threw away every occluder more than one radius nearer the camera (its range check). A thin blade in
    // front of the ground under a meadow is exactly such an occluder, so the floor of a grass field measured ~0.8 and
    // was lit by the whole sky. The horizon search asks the question AO is defined by - how much of the cosine
    // hemisphere does the nearest horizon in each direction hide - and the thickness heuristic lets a thin occluder
    // hide less than a wall without dropping it.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        #include <Common/ReconstructPosition.glslh>

        In(0) vec2 v_TexCoord;

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler u_PointClampSampler;
        Uniform(4) sampler u_LinearRepeatSampler;
        Uniform(1) texture2D u_GBufferDepth;  // r = device depth (nearest sampler: DEPTH32F is not filtered)
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        Uniform(2) texture2D u_GBufferNormal; // rgb = world normal
        #define u_GBufferNormal sampler2D(u_GBufferNormal, u_LinearRepeatSampler)

        Out(0) vec4 oAO;

        Uniform(0) SSAOUB
        {
        	mat4 InvJitteredViewProjection; // the inverse of the matrix the G-buffer depth was rasterised with
        	mat4 WorldToView;               // the camera's View matrix (right-handed, -z forward)
        	vec4 Target;                    // xy = AO target size in pixels, zw = 1 / size
        	vec4 Horizon;                   // x = world radius (cm), y = thickness blend (UE-remapped), z = taps per
        	                                //     direction, w = pixels per world unit at view depth 1
        	vec4 Temporal;                  // xy = cos/sin of this frame's slice rotation, z = step offset, w = angles
        };

        const float kPiHalf             = 1.5707963;
        const float kPi                 = 3.1415927;
        const float kMaxPixelRadius     = 256.0; // UE GTAO_MAX_PIXEL_SCREEN_RADIUS
        const uint  kMaxTaps            = 32u;

        // UE's view space from a world position: x right, y up, +z forward (the View matrix looks down -z).
        vec3 ToViewUE(vec3 world)
        {
        	const vec3 v = vec3(WorldToView * vec4(world, 1.0));
        	return vec3(v.x, v.y, -v.z);
        }

        // UE ScreenToViewPos through our depth: the texel's world position, in UE's view space. false = sky.
        bool ViewPosAt(vec2 uv, out vec3 viewPos)
        {
        	const float deviceZ = texture(u_GBufferDepth, uv).r;
        	if (deviceZ <= 0.0)
        	{
        		viewPos = vec3(0.0);
        		return false;
        	}
        	viewPos = ToViewUE(ReconstructWorldPosition(uv, deviceZ, InvJitteredViewProjection));
        	return true;
        }

        // UE SearchForLargestAngleDual: the cosine of the highest horizon on each side of the slice.
        vec2 SearchForLargestAngleDual(uint numSteps, vec2 baseUV, vec2 screenDir, float searchRadius,
                                       float initialOffset, vec3 viewPos, vec3 viewDir, float attenFactor)
        {
        	vec2        bestAng   = vec2(-1.0, -1.0);
        	const float thickness = Horizon.y;

        	for (uint i = 0u; i < numSteps; i++)
        	{
        		const float fi       = float(i);
        		vec2        uvOffset = screenDir * max(searchRadius * (fi + initialOffset), fi + 1.0);
        		uvOffset.y *= -1.0;
        		const vec4 uv2 = baseUV.xyxy + vec4(uvOffset.xy, -uvOffset.xy);

        		vec3 samplePos;
        		if (ViewPosAt(uv2.xy, samplePos))
        		{
        			const vec3  v       = samplePos - viewPos;
        			const float lenSq   = dot(v, v);
        			float       ang     = dot(v, viewDir) * inversesqrt(lenSq + 0.0001);
        			const float fallOff = clamp(lenSq * attenFactor, 0.0, 1.0);
        			ang       = mix(ang, bestAng.x, fallOff);
        			bestAng.x = (ang > bestAng.x) ? ang : mix(ang, bestAng.x, thickness);
        		}
        		if (ViewPosAt(uv2.zw, samplePos))
        		{
        			const vec3  v       = samplePos - viewPos;
        			const float lenSq   = dot(v, v);
        			float       ang     = dot(v, viewDir) * inversesqrt(lenSq + 0.0001);
        			const float fallOff = clamp(lenSq * attenFactor, 0.0, 1.0);
        			ang       = mix(ang, bestAng.y, fallOff);
        			bestAng.y = (ang > bestAng.y) ? ang : mix(ang, bestAng.y, thickness);
        		}
        	}
        	return vec2(acos(clamp(bestAng.x, -1.0, 1.0)), acos(clamp(bestAng.y, -1.0, 1.0)));
        }

        // UE ComputeInnerIntegral: the cosine-weighted visible arc of the slice between the two horizons, with the
        // normal projected onto the slice plane.
        float ComputeInnerIntegral(vec2 angles, vec2 screenDir, vec3 viewDir, vec3 viewSpaceNormal)
        {
        	const vec3 planeNormal = normalize(cross(vec3(screenDir.xy, 0.0), viewDir));
        	const vec3 perp        = cross(viewDir, planeNormal);
        	const vec3 projNormal  = viewSpaceNormal - planeNormal * dot(viewSpaceNormal, planeNormal);

        	const float lenProjNormal = length(projNormal) + 0.000001;
        	const float recipMag      = 1.0 / lenProjNormal;

        	const float cosAng   = dot(projNormal, perp) * recipMag;
        	const float gamma    = acos(clamp(cosAng, -1.0, 1.0)) - kPiHalf;
        	const float cosGamma = dot(projNormal, viewDir) * recipMag;
        	const float sinGamma = cosAng * -2.0;

        	// Clamp to the normal's hemisphere.
        	angles.x = gamma + max(-angles.x - gamma, -kPiHalf);
        	angles.y = gamma + min(angles.y - gamma, kPiHalf);

        	return lenProjNormal * 0.25 *
        	       ((angles.x * sinGamma + cosGamma - cos((2.0 * angles.x) - gamma)) +
        	        (angles.y * sinGamma + cosGamma - cos((2.0 * angles.y) - gamma)));
        }

        float InterleavedGradientNoise(vec2 iPos)
        {
        	return fract(52.9829189 * fract((iPos.x * 0.06711056) + (iPos.y * 0.00583715)));
        }

        // UE GetRandomVector: the slice direction (interleaved gradient noise, rotated by this frame's angle) and the
        // step offset (a 4x4 pattern plus this frame's offset).
        vec3 GetRandomVector(uvec2 iPos)
        {
        	iPos.y = 16384u - iPos.y;
        	const float gradientNoise = InterleavedGradientNoise(vec2(iPos));
        	const vec2  randomTexVec  = vec2(cos(gradientNoise * kPi), sin(gradientNoise * kPi));
        	const float scaleOffset   = (1.0 / 4.0) * float((iPos.y - iPos.x) & 3u);

        	vec3 randomVec;
        	randomVec.x = dot(randomTexVec, vec2(Temporal.x, -Temporal.y));
        	randomVec.y = dot(randomTexVec, vec2(Temporal.y, Temporal.x));
        	randomVec.z = fract(scaleOffset + Temporal.z);
        	return randomVec;
        }

        void main()
        {
        	const vec3 worldNormal = texture(u_GBufferNormal, v_TexCoord).rgb;
        	vec3       viewSpacePos;
        	// No geometry (the G-buffer cleared to 0, or the sky's depth): unoccluded.
        	if (dot(worldNormal, worldNormal) <= 0.001 || !ViewPosAt(v_TexCoord, viewSpacePos))
        	{
        		oAO = vec4(1.0);
        		return;
        	}

        	const vec3 n               = mat3(WorldToView) * normalize(worldNormal);
        	const vec3 viewSpaceNormal = vec3(n.x, n.y, -n.z);
        	const vec3 viewDir         = normalize(-viewSpacePos);

        	const uint  numTaps     = min(uint(Horizon.z + 0.5), kMaxTaps);
        	const float worldRadius = Horizon.x;
        	// The radius in pixels at this depth, between the tap count and UE's screen cap.
        	const float pixelRadius = max(min(worldRadius * Horizon.w / viewSpacePos.z, kMaxPixelRadius), float(numTaps));
        	const float stepRadius  = pixelRadius / (float(numTaps) + 1.0);
        	const float attenFactor = 2.0 / (worldRadius * worldRadius);

        	const vec3 randomAndOffset = GetRandomVector(uvec2(gl_FragCoord.xy));
        	vec2       screenDir       = randomAndOffset.xy;
        	float      offset          = randomAndOffset.z;

        	const uint  numAngles     = uint(Temporal.w + 0.5);
        	const float sinDeltaAngle = sin(kPi / float(numAngles));
        	const float cosDeltaAngle = cos(kPi / float(numAngles));

        	float sum = 0.0;
        	for (uint a = 0u; a < numAngles; a++)
        	{
        		const vec2 angles = SearchForLargestAngleDual(numTaps, v_TexCoord, screenDir * Target.zw, stepRadius, offset,
        		                                              viewSpacePos, viewDir, attenFactor);
        		sum += ComputeInnerIntegral(angles, screenDir, viewDir, viewSpaceNormal);

        		// Rotate for the next angle.
        		const vec2 previous = screenDir;
        		screenDir.x         = (previous.x * cosDeltaAngle) + (previous.y * -sinDeltaAngle);
        		screenDir.y         = (previous.x * sinDeltaAngle) + (previous.y * cosDeltaAngle);
        		offset              = fract(offset + 0.617);
        	}

        	oAO = vec4(vec3(clamp(sum / float(numAngles), 0.0, 1.0)), 1.0);
        }
    }
}

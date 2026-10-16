// DesertAsset {"Kind":"Shader","Guid":"a1ace64b6cb84e3588046ff8d330f5aa","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDTemporalAccumulation"
{
    // The screen-space denoiser's TEMPORAL ACCUMULATION, every signal (UE SSDTemporalAccumulation.usf).
    //  1) REPROJECTION. The history is read where the pixel's signal was last frame. For reflections that is the
    //     REFLECTED point (UE's reflection reprojection): what lies d beyond the surface along the view ray (the
    //     virtual image a mirror forms; d = the coverage-weighted distance to the hit points of the 2x2 trace
    //     texels nearest the pixel, u_Trace / u_TraceRay), projected through last frame's camera - a reflection
    //     stays on what it reflects while the camera moves; with nothing hit, the point at infinity. For diffuse
    //     light d = 0: the surface point itself. The surface's own motion (its velocity less the camera's share)
    //     is added, so a moving surface carries its signal.
    //  2) REJECTION (UE HISTORY_REJECTION_VAR_BOUNDARIES): the history is clamped, in luma/chroma (YCoCg) and
    //     coverage, to the mean +- 1.25 standard deviations of this frame's neighbourhood (5x5, 2 px apart); the further the
    //     clamp moved it, the fewer of its samples are kept (UE's rejection factor: the log-luma change, the
    //     chroma change, the coverage change). A disoccluded or changed signal restarts instead of ghosting.
    //  3) ACCUMULATION BY SAMPLE COUNT (UE TargetedSampleCount): the history carries how many frames it holds
    //     (u_HistoryCount); a frame adds one, and the history may keep at most the target - 1. Reflections target
    //     the specular lobe's footprint in pixels (a sharp lobe needs few samples, a mirror's would be one - it
    //     cannot average what it does not blur), floored at u_Params.w; diffuse light targets u_Params.z.
    //     new = (history * kept + current) / (kept + 1); count = kept + 1.
    // Everything is premultiplied (rgb * a, a) - Common/ScreenSpaceDenoise.glslh.

    Compute
    {
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest)
        Uniform(3) sampler2D u_Signal;          // this frame's reconstruction, premultiplied
        Uniform(4) sampler2D u_Trace;           // reflections: SSR's half-res trace (a = coverage); diffuse: black
        Uniform(6) sampler2D u_TraceRay;        // reflections: its hit points (xyz); diffuse: black
        Uniform(5) sampler2D u_History;         // last frame's accumulation, premultiplied (linear)
        Uniform(7) sampler2D u_HistoryCount;    // last frame's sample count (nearest)
        Uniform(8) sampler2D u_Velocity;        // NDC current - previous per pixel (View/Velocity.hpp), unjittered
        layout(binding = 9, rgba16f) writeonly uniform image2D u_NewHistory;      // premultiplied
        layout(binding = 10, r32f) writeonly uniform image2D u_NewHistoryCount;

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 u_ViewProjection;            // THIS frame's world -> clip, unjittered (the velocity's space)
        	mat4 u_PrevViewProjection;        // LAST frame's world -> clip, unjittered
        	mat4 u_InvJitteredViewProjection; // THIS frame's: world position from u_GBufferDepth
        };

        PushConstant PushConstants
        {
        	// x = 1 when u_History holds the view's previous frame, y = signal (kSSDSignal*),
        	// z = the most frames a history may hold, w = the fewest a reflection targets
        	vec4 u_Params;
        };

        LocalSize(8, 8, 1);

        // World point (w = 1) or direction (w = 0, a point at infinity) -> NDC xy through @p viewProj; false
        // behind that camera.
        bool ProjectNdc(mat4 viewProj, vec4 p, out vec2 ndc)
        {
        	const vec4 clip = viewProj * p;
        	if (clip.w <= 0.0)
        		return false;
        	ndc = clip.xy / clip.w;
        	return true;
        }

        vec4 ToRejectionSpace(vec4 s)
        {
        	return vec4(SSDRGBToYCoCg(s.rgb), s.a);
        }

        void main()
        {
        	const ivec2 viewSize = textureSize(u_GBufferNormal, 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	const int signal = int(u_Params.y + 0.5);
        	if (!SSDPixelHasSignal(signal, texelFetch(u_GBufferNormal, pixel, 0)))
        	{
        		imageStore(u_NewHistory, pixel, vec4(0.0));
        		imageStore(u_NewHistoryCount, pixel, vec4(0.0));
        		return;
        	}
        	const vec4 current = texelFetch(u_Signal, pixel, 0);

        	// --- The neighbourhood's first and second moments: 5x5 taps two pixels apart. The reconstruction shares
        	// most of its trace texels between adjacent pixels, so a 3x3 box of it (UE's rejection set, taken over
        	// decorrelated samples) is spatially correlated: its spread is far below the frame-to-frame noise and
        	// the box would reject converged history every frame. Taps that far apart read other texel sets. ---
        	vec4 m1 = vec4(0.0);
        	vec4 m2 = vec4(0.0);
        	for (int y = -2; y <= 2; ++y)
        		for (int x = -2; x <= 2; ++x)
        		{
        			const vec4 s = ToRejectionSpace(
        			     texelFetch(u_Signal, clamp(pixel + ivec2(x, y) * 2, ivec2(0), viewSize - 1), 0));
        			m1 += s;
        			m2 += s * s;
        		}
        	m1 /= 25.0;
        	m2 /= 25.0;
        	const vec4 stdDev = sqrt(abs(m2 - m1 * m1));
        	const vec4 boxMin = m1 - 1.25 * stdDev;
        	const vec4 boxMax = m1 + 1.25 * stdDev;

        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth, u_InvJitteredViewProjection,
        	                                           pixel, size);
        	const vec3         V    = SSDViewVector(uv, ref.Position, u_InvJitteredViewProjection);

        	// --- The targeted sample count. ---
        	float target = u_Params.z;
        	if (signal == kSSDSignalReflections)
        	{
        		const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(u_GBufferDepth, pixel, 0).r, ref.Position,
        		                                              1.0 / size, u_InvJitteredViewProjection);
        		const vec3  nearPoint   = ReconstructWorldPosition(uv, 1.0, u_InvJitteredViewProjection);
        		const vec2  radii       = SSDLobePixelRadii(ref.Roughness, abs(dot(ref.Normal, V)), pixelRadius,
        		                                            distance(nearPoint, ref.Position));
        		target = clamp(kSSDPi * radii.x * radii.y, u_Params.w, u_Params.z);
        	}

        	// --- Reprojection: the reflected point (d = 0: the surface) plus the surface's own motion. ---
        	float historyCount = 0.0;
        	vec4  history      = vec4(0.0);
        	if (u_Params.x > 0.5)
        	{
        		// The distance to what the pixel reflects: the hits of the 2x2 trace texels around it, weighed by
        		// their coverage (diffuse: no trace, d = 0 - the surface itself).
        		float       d         = 0.0;
        		{
        			const ivec2 traceSize = textureSize(u_Trace, 0);
        			const vec2  t0        = vec2(pixel) * (vec2(traceSize) / size) - 0.5;
        			float       dSum = 0.0, aSum = 0.0;
        			for (int j = 0; j <= 1; ++j)
        				for (int i = 0; i <= 1; ++i)
        				{
        					const ivec2 t = clamp(ivec2(floor(t0)) + ivec2(i, j), ivec2(0), traceSize - 1);
        					const float a = texelFetch(u_Trace, t, 0).a;
        					dSum += a * distance(texelFetch(u_TraceRay, t, 0).xyz, ref.Position);
        					aSum += a;
        				}
        			d = aSum > 0.0 ? dSum / aSum : 0.0;
        		}
        		const vec4  reflected = signal == kSSDSignalReflections && current.a <= 0.0
        		                             ? vec4(-V, 0.0)
        		                             : vec4(ref.Position - V * d, 1.0);
        		vec2 surfaceNow, surfacePrev, reflectedPrev;
        		if (ProjectNdc(u_ViewProjection, vec4(ref.Position, 1.0), surfaceNow) &&
        		    ProjectNdc(u_PrevViewProjection, vec4(ref.Position, 1.0), surfacePrev) &&
        		    ProjectNdc(u_PrevViewProjection, reflected, reflectedPrev))
        		{
        			const vec2 objectMotion = texelFetch(u_Velocity, pixel, 0).xy - (surfaceNow - surfacePrev);
        			const vec2 prevNdc      = reflectedPrev - objectMotion;
        			const vec2 prevUV       = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5); // Y-flipped
        			if (prevUV.x > 0.0 && prevUV.x < 1.0 && prevUV.y > 0.0 && prevUV.y < 1.0)
        			{
        				history      = textureLod(u_History, prevUV, 0.0);
        				historyCount = texelFetch(u_HistoryCount, ivec2(prevUV * size), 0).r;
        			}
        		}
        	}

        	// --- Rejection: clamp to the variance box, keep fewer samples the further it moved. ---
        	if (historyCount > 0.0)
        	{
        		const vec4 transformed = ToRejectionSpace(history);
        		const vec4 clamped     = clamp(transformed, boxMin, boxMax);
        		const vec4 diff        = abs(clamped - transformed);
        		float rejection = abs(log2(1.0 + max(clamped.x, 0.0)) - log2(1.0 + max(transformed.x, 0.0)));
        		rejection = max(rejection, max(diff.y, diff.z) / (1.0 + max(clamped.x, 0.0)));
        		rejection = max(rejection, diff.w);
        		historyCount *= clamp(1.0 - rejection, 0.0, 1.0);
        		// Back to RGB: Y + Co - Cg, Y + Cg, Y - Co - Cg.
        		history = vec4(clamped.x + clamped.y - clamped.z, clamped.x + clamped.z, clamped.x - clamped.y - clamped.z,
        		               clamped.w);
        		history = max(history, vec4(0.0));
        	}

        	// --- Accumulation by sample count. ---
        	const float kept = min(historyCount, max(target - 1.0, 0.0));
        	const vec4  next = (history * kept + current) / (kept + 1.0);
        	imageStore(u_NewHistory, pixel, next);
        	imageStore(u_NewHistoryCount, pixel, vec4(kept + 1.0));
        }
    }
}

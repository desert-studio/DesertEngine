// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections (fullscreen). Traces one jittered ray per pixel through the G-buffer and
    // samples the composited scene colour at the hit; SSRResolve denoises it and SSRComposite blends it.

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        // Screen-space reflections (SSR). For smooth opaque pixels, reflects the view ray off the surface, marches
        // it through the G-buffer in world space (projecting each step to screen) and samples the composited scene
        // colour where it hits — so mirrors/metal/polished floors reflect the on-screen room.
        // Output is BLENDED over the scene: rgb = reflected colour, a = reflectance (Fresnel * smoothness * hit fade).
        // Screen-space => reflections of off-screen / occluded geometry are missed (standard SSR limitation).
        //
        // March: coarse pass with a slightly growing step over u_SSRParams.y world units, then a short binary
        // refinement between the last two points to pin the hit texel (kills the banding a coarse-only march has).

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_GBufferAlbedo;   // rgb = albedo, a = metallic
        Uniform(2) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(3) sampler2D u_GBufferWorldPos; // rgb = world position
        Uniform(4) sampler2D u_SceneColor;      // composited opaque scene (reflection source)

        Out(0) vec4 oColor;

        Uniform(0) SSRUB
        {
        	mat4 u_ViewProj;   // world -> clip (to project the marched ray to screen)
        	vec4 u_CameraPos;  // xyz = camera world position, w = per-frame jitter seed (temporal accumulation)
        	vec4 u_SSRParams;  // x = max steps, y = max ray distance (world), z = intensity, w = thickness (world)
        };

        // Per-pixel hash (same one SSAO uses) — jitters the ray start so the coarse march's banding turns into
        // fine noise, which the composite pass's blur then resolves into a smooth reflection.
        float hash12(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // World point -> screen UV. Returns uv in [0,1]; ok=false when behind the camera.
        bool projectToScreen(vec3 p, out vec2 uv)
        {
        	vec4 clip = u_ViewProj * vec4(p, 1.0);
        	if (clip.w <= 0.0)
        		return false;
        	uv = clip.xy / clip.w * 0.5 + 0.5;
        	uv.y = 1.0 - uv.y; // engine renders through a Y-flipped viewport (same convention as SSAO)
        	return true;
        }

        // Signed "ray point is this far behind the G-buffer surface at its texel" (camera-radial). Positive =
        // the ray crossed into geometry; sky texels report a huge negative (never a crossing).
        float depthDelta(vec3 p, vec2 uv)
        {
        	vec3 sN = texture(u_GBufferNormal, uv).rgb;
        	if (dot(sN, sN) <= 0.001)
        		return -1e9; // sky — no occluder here
        	vec3 sPos = texture(u_GBufferWorldPos, uv).rgb;
        	return distance(u_CameraPos.xyz, p) - distance(u_CameraPos.xyz, sPos);
        }

        void main()
        {
        	vec4 gb = texture(u_GBufferNormal, v_TexCoord);
        	vec3 N  = gb.rgb;
        	if (dot(N, N) <= 0.001) { oColor = vec4(0.0); return; } // sky / no geometry

        	float metallic   = texture(u_GBufferAlbedo, v_TexCoord).a;
        	float roughness  = gb.a;
        	float smoothness = 1.0 - roughness;
        	// Rough surfaces reflect too diffusely for a sharp SSR ray — fade them out early (soft gate, not a
        	// metal-only cutoff: a smooth dielectric floor still gets its Fresnel reflection like in UE).
        	float smoothFade = smoothstep(0.4, 0.7, smoothness);
        	if (smoothFade < 0.01) { oColor = vec4(0.0); return; }

        	N = normalize(N);
        	vec3 worldPos = texture(u_GBufferWorldPos, v_TexCoord).rgb;
        	vec3 V = normalize(u_CameraPos.xyz - worldPos);
        	vec3 R = reflect(-V, N);

        	// Schlick with a metalness-lerped F0: dielectrics reflect at grazing angles, metals everywhere.
        	float f0       = mix(0.04, 0.9, metallic);
        	float fresnel  = f0 + (1.0 - f0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);
        	float strength = fresnel * smoothFade * u_SSRParams.z;
        	if (strength < 0.01) { oColor = vec4(0.0); return; }

        	int   maxSteps  = int(u_SSRParams.x);
        	float maxDist   = u_SSRParams.y;
        	float thickness = u_SSRParams.w;

        	// Slightly growing step: fine contact reflections near the surface, long reach further out.
        	// Geometric series sized so all maxSteps steps sum EXACTLY to maxDist, last step = 8x the first.
        	float grow  = pow(8.0, 1.0 / float(maxSteps));
        	float step0 = maxDist * (grow - 1.0) / (pow(grow, float(maxSteps)) - 1.0);

        	vec3  hitColor = vec3(0.0);
        	float hit      = 0.0;
        	float stepLen  = step0;
        	float t        = 0.0;
        	// Start slightly off the surface to avoid self-hit. 2.0 is WORLD units, i.e. two CENTIMETRES:
        	// the metre-era 0.02 (= 2 cm then) survived the unit switch as 0.2 mm, far below the depth
        	// deltas a G-buffer texel carries at centimetre scale, so grazing rays began "inside" their own
        	// surface and were rejected by the very first sign-change test.
        	vec3  pPrev    = worldPos + N * 2.0;
        	// Jitter the start by a random fraction of the first step — DIFFERENT each frame (the seed in
        	// CameraPos.w) so the temporal accumulation averages a fresh estimate every frame and converges.
        	pPrev += R * ( step0 * hash12(v_TexCoord * 4096.0 + vec2(u_CameraPos.w)) );

        	// Crossing = the ray's depth delta changes SIGN between two samples (in front of the surface ->
        	// behind it). Detecting by sign change instead of a "within a thickness band" test is what removes
        	// the black march-quantization rings on curved reflectors: at grazing incidence the radial delta can
        	// jump PAST any band in one step, which used to discard the crossing and leave a dark ring.
        	float deltaPrev = -1.0;
        	for (int i = 0; i < maxSteps && t < maxDist; i++)
        	{
        		vec3 p = pPrev + R * stepLen;
        		t += stepLen;

        		vec2 suv;
        		if (!projectToScreen(p, suv)) break;
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;

        		float delta = depthDelta(p, suv);
        		if (deltaPrev <= 0.0 && delta > 0.0)
        		{
        			// Binary refinement between pPrev (outside) and p (inside) — pins the exact crossing point.
        			vec3 lo = pPrev, hi = p;
        			for (int j = 0; j < 6; j++)
        			{
        				vec3 mid = 0.5 * (lo + hi);
        				vec2 muv;
        				if (!projectToScreen(mid, muv)) break;
        				if (depthDelta(mid, muv) > 0.0) hi = mid; else lo = mid;
        			}
        			vec3 hitP = 0.5 * (lo + hi);
        			vec2 huv;
        			if (!projectToScreen(hitP, huv)) break;

        			// A TRUE surface crossing refines to a point ~on the surface (tiny |delta|). A large refined
        			// delta means the ray slipped BEHIND an object's silhouette into the depth gap — that is not
        			// a hit: keep marching (it may legitimately strike something further along).
        			float refined = depthDelta(hitP, huv);
        			vec3  hN      = texture(u_GBufferNormal, huv).rgb;
        			const bool backface = dot(hN, hN) > 0.001 && dot(normalize(hN), R) > 0.2;
        			if (abs(refined) < thickness && !backface)
        			{
        				hitColor = texture(u_SceneColor, huv).rgb;
        				// Firefly clamp: reflections of blown-out HDR texels (sun-lit wall ~10x) otherwise
        				// produce single bright dots with contrast no spatial blur can hide.
        				float peak = max(hitColor.r, max(hitColor.g, hitColor.b));
        				if (peak > 3.0)
        					hitColor *= 3.0 / peak;
        				// Fade near the screen edge (no popping) and by ray travel (soft range limit).
        				vec2 edge = smoothstep(vec2(0.0), vec2(0.1), huv) * (1.0 - smoothstep(vec2(0.9), vec2(1.0), huv));
        				hit = edge.x * edge.y * (1.0 - smoothstep(0.7 * maxDist, maxDist, t));
        				break;
        			}
        			// else: silhouette gap / backface — fall through and continue the march.
        		}

        		deltaPrev = delta;
        		pPrev     = p;
        		stepLen  *= grow;
        	}

        	oColor = vec4(hitColor, clamp(hit * strength, 0.0, 1.0));
        }
    }
}

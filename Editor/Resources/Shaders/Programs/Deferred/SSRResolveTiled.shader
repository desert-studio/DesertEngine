// DesertAsset {"Kind":"Shader","Guid":"a607db1b6c40417bbe22f774e11ba5bd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRResolveTiled"
{
    // SSR's resolve, drawn only over the tiles next to a pixel the trace can write (Common/SSRTiles.glslh).
    // Everywhere else its output would be exactly 0, which is what the accumulation target is cleared to.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>
        #include <Common/SSRTiles.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	gl_Position = SSRTileVertex(1, v_TexCoord);
        }
    }

    Fragment
    {
        // 1) SPATIAL - the RATIO ESTIMATOR over the neighbours (Stachowiak 2015, "Stochastic Screen-Space
        //    Reflections"; UE SSRT's reflection reconstruction): each of the 3x3 half-resolution trace texels
        //    around this pixel is one sample of THIS pixel's lobe. The texel gives its HIT POINT (u_TraceRay xyz)
        //    and the log2 pdf with which it drew its ray (w); the direction is re-aimed from THIS pixel's surface
        //    point at that hit (UE SSRT), so a neighbour on a slightly different spot of the surface lends the
        //    ray this pixel would have needed to reach the same point - the reflection of a near object does not
        //    smear by the neighbour's offset. Weight = this pixel's BRDF * cos of that direction over the
        //    neighbour's pdf. A mirror's BRDF is ~0 away from its own direction, so it borrows nothing and stays
        //    sharp. Fresnel is excluded, as in the trace (the lighting's split sum multiplies it).
        //    rgb = sum(w * a * k * L) / sum(w * a * k) (k = Karis' inverse luminance, as in the trace),
        //    a   = sum(w * a) / sum(w): the share of THIS pixel's lobe the hits answered.
        // 2) TEMPORAL - last frame's result where the REFLECTED point was (UE's reflection reprojection): the
        //    reflection of a hit at distance d is seen as if it lay d beyond the surface along the view ray (the
        //    virtual image a mirror forms), and THAT point - not the surface - is projected through last frame's
        //    camera; a reflection then stays on what it reflects while the camera moves (reprojecting through the
        //    surface's velocity slides it with the surface: the parallax trail). The surface's own motion (its
        //    velocity less the camera's share) is added, so a moving mirror carries its reflection. Clamped to
        //    the neighbourhood box (Common/SSRDenoise.glslh SSRTemporalBlend): a moving REFLECTED object's old
        //    image is outside the box and cannot trail.
        #include <Common/SSRDenoise.glslh>
        #include <Common/SSRGate.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Trace;           // this frame's half-resolution estimate (rgb, a = coverage)
        Uniform(2) sampler2D u_History;         // previous frame's resolved result
        Uniform(3) sampler2D u_GBufferDepth;    // device depth (nearest): this pixel's world position
        Uniform(5) sampler2D u_TraceRay;        // the trace texels' hit points + pdfs (SSR.shader u_TraceRay)
        Uniform(6) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(7) sampler2D u_Velocity;        // NDC current - previous per pixel (View/Velocity.hpp), unjittered

        Out(0) vec4 oColor;

        // SSR's own block (MaterialSSRReflectionResolve::SSRReflectionResolveUBData), not the GI resolve's.
        Uniform(0) SSRReflectionResolveUB
        {
        	mat4 u_ViewProjection;     // THIS frame's world -> clip, unjittered (the velocity's space)
        	mat4 u_PrevViewProjection; // LAST frame's world -> clip, unjittered
        	mat4 u_InvJitteredViewProjection; // THIS frame's: world position from u_GBufferDepth
        	vec4 u_Params;       // xy = texel size, z = history blend (0 = first frame / resize), w unused
        };

        const float kPi = 3.14159265358979;

        float SmithG1(float nDotX, float a2)
        {
        	return 2.0 * nDotX / (nDotX + sqrt(a2 + (1.0 - a2) * nDotX * nDotX));
        }

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

        void main()
        {
        	const ivec2 pixel = ivec2(gl_FragCoord.xy);
        	const vec4  gb    = texelFetch(u_GBufferNormal, pixel, 0);
        	if (!SSRPixelCanReflect(gb))
        	{
        		oColor = vec4(0.0);
        		return;
        	}
        	const vec2  uv    = v_TexCoord;
        	const vec3  N     = normalize(gb.rgb);
        	const float alpha = max(gb.a * gb.a, 0.001);
        	const float a2    = alpha * alpha;
        	const vec3  P     = ReconstructWorldPosition(uv, texelFetch(u_GBufferDepth, pixel, 0).r,
        	                                             u_InvJitteredViewProjection);
        	// Toward the camera: the pixel's own ray, from the surface to its near-plane point (reversed-Z, 1 = near:
        	// Core/Projection.hpp) - the camera position is on that line, so no camera uniform is needed.
        	const vec3  V     = normalize(ReconstructWorldPosition(uv, 1.0, u_InvJitteredViewProjection) - P);
        	const float nDotV = max(dot(N, V), 1e-4);
        	const float g1V   = SmithG1(nDotV, a2);

        	const ivec2 traceSize = textureSize(u_Trace, 0);
        	const ivec2 centre    = clamp(ivec2(uv * vec2(traceSize)), ivec2(0), traceSize - 1);
        	vec3  radiance  = vec3(0.0);
        	float hitWeight = 0.0;
        	float coverage  = 0.0;
        	float weightSum = 0.0;
        	float hitDistance = 0.0; // sum of w * a * |hit - P|: the distance of what this pixel reflects
        	vec3  mnC = vec3(1e9), mxC = vec3(-1e9);
        	float mnA = 1e9, mxA = -1e9;
        	for (int y = -1; y <= 1; ++y)
        		for (int x = -1; x <= 1; ++x)
        		{
        			const ivec2 t     = clamp(centre + ivec2(x, y), ivec2(0), traceSize - 1);
        			const vec4  trace = texelFetch(u_Trace, t, 0);
        			const vec4  ray   = texelFetch(u_TraceRay, t, 0);
        			// The neighbourhood box of the temporal clamp: every texel, as the GI denoiser builds it.
        			const vec3 c = trace.a > 0.001 ? trace.rgb : vec3(0.0);
        			mnC = min(mnC, c);
        			mxC = max(mxC, c);
        			mnA = min(mnA, trace.a);
        			mxA = max(mxA, trace.a);
        			if (ray.w <= -125.0)
        				continue; // the texel drew no ray (it cannot reflect, or its trace was lost): nothing to reuse
        			// Re-aimed from THIS pixel at the neighbour's hit point (UE SSRT).
        			const vec3  toHit = ray.xyz - P;
        			const float dist  = length(toHit);
        			if (dist <= 0.0)
        				continue;
        			const vec3  L     = toHit / dist;
        			const float nDotL = dot(N, L);
        			if (nDotL <= 0.0)
        				continue; // below this pixel's horizon: outside its lobe
        			const vec3  H     = normalize(V + L);
        			const float nDotH = max(dot(N, H), 0.0);
        			const float d     = nDotH * nDotH * (a2 - 1.0) + 1.0;
        			const float D     = a2 / (kPi * d * d);
        			// BRDF * cos without Fresnel: D * G1(V) * G1(L) / (4 N.V), over the drawing texel's pdf.
        			const float w = D * g1V * SmithG1(nDotL, a2) / (4.0 * nDotV) * exp2(-ray.w);
        			const float k = 1.0 / (1.0 + dot(trace.rgb, vec3(0.2126, 0.7152, 0.0722)));
        			weightSum += w;
        			coverage  += w * trace.a;
        			hitDistance += w * trace.a * dist;
        			radiance  += trace.rgb * (w * trace.a * k);
        			hitWeight += w * trace.a * k;
        		}
        	vec4 cur = vec4(0.0);
        	if (weightSum > 0.0 && hitWeight > 0.0)
        		cur = vec4(radiance / hitWeight, coverage / weightSum);

        	if (u_Params.z <= 0.0)
        	{
        		oColor = cur;
        		return;
        	}
        	// The reflected point: d beyond the surface along the view ray (a point at infinity when nothing was hit:
        	// what the next sources answer, the environment, is infinitely far).
        	const vec4 reflected = coverage > 0.0 ? vec4(P - V * (hitDistance / coverage), 1.0) : vec4(-V, 0.0);
        	// The surface's own motion: its velocity less the share the camera's move gives a still point there.
        	vec2 surfaceNow, surfacePrev, reflectedPrev;
        	if (!ProjectNdc(u_ViewProjection, vec4(P, 1.0), surfaceNow) ||
        	    !ProjectNdc(u_PrevViewProjection, vec4(P, 1.0), surfacePrev) ||
        	    !ProjectNdc(u_PrevViewProjection, reflected, reflectedPrev))
        	{
        		oColor = cur; // the point was behind last frame's camera: no history of it
        		return;
        	}
        	const vec2 objectMotion = texelFetch(u_Velocity, pixel, 0).xy - (surfaceNow - surfacePrev);
        	const vec2 prevNdc      = reflectedPrev - objectMotion;
        	const vec2 prevUV       = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5); // Y-flipped viewport
        	oColor = SSRTemporalBlend(cur, u_History, prevUV, mnC, mxC, mnA, mxA, u_Params.z);
        }
    }
}

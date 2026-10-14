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
        //    around this pixel is one sample of THIS pixel's lobe - its representative ray (u_TraceRay: direction
        //    + log2 pdf of the texel that drew it) weighted by this pixel's BRDF * cos over that pdf. A glossy
        //    pixel borrows its neighbours' rays as its own (9x the rays, no blur beyond the lobe); a mirror's BRDF
        //    is ~0 away from its own direction, so it borrows nothing and stays sharp. Fresnel is excluded, as in
        //    the trace (the lighting's split sum multiplies it).
        //    rgb = sum(w * a * k * L) / sum(w * a * k) (k = Karis' inverse luminance, as in the trace),
        //    a   = sum(w * a) / sum(w): the share of THIS pixel's lobe the hits answered.
        // 2) TEMPORAL - last frame's result where this pixel was: the pixel minus its VELOCITY (object motion as
        //    well as camera motion), clamped to the neighbourhood box (Common/SSRDenoise.glslh SSRTemporalBlend).
        #include <Common/SSRDenoise.glslh>
        #include <Common/SSRGate.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Trace;           // this frame's half-resolution estimate (rgb, a = coverage)
        Uniform(2) sampler2D u_History;         // previous frame's resolved result
        Uniform(3) sampler2D u_GBufferDepth;    // device depth (nearest): this pixel's world position
        Uniform(5) sampler2D u_TraceRay;        // the trace texels' representative rays (SSR.shader u_TraceRay)
        Uniform(6) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(7) sampler2D u_Velocity;        // NDC current - previous per pixel (View/Velocity.hpp)

        Out(0) vec4 oColor;

        Uniform(0) SSRResolveUB
        {
        	mat4 u_PrevViewProj; // LAST frame's world -> clip (the GI variant's reprojection; SSR's is the velocity)
        	mat4 u_InvJitteredViewProjection; // THIS frame's: world position from u_GBufferDepth
        	vec4 u_Params;       // xy = texel size, z = history blend (0 = first frame / resize), w unused
        };

        const float kPi = 3.14159265358979;

        float SmithG1(float nDotX, float a2)
        {
        	return 2.0 * nDotX / (nDotX + sqrt(a2 + (1.0 - a2) * nDotX * nDotX));
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
        			if (ray.w <= -125.0 || dot(ray.xyz, ray.xyz) < 0.5)
        				continue; // the texel drew no ray (it cannot reflect, or its trace was lost): nothing to reuse
        			const vec3  L     = normalize(ray.xyz);
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
        			radiance  += trace.rgb * (w * trace.a * k);
        			hitWeight += w * trace.a * k;
        		}
        	vec4 cur = vec4(0.0);
        	if (weightSum > 0.0 && hitWeight > 0.0)
        		cur = vec4(radiance / hitWeight, coverage / weightSum);

        	const vec2 prevUV = uv - texelFetch(u_Velocity, pixel, 0).xy * vec2(0.5, -0.5);
        	oColor = u_Params.z > 0.0 ? SSRTemporalBlend(cur, u_History, prevUV, mnC, mxC, mnA, mxA, u_Params.z) : cur;
        }
    }
}

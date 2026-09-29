// DesertAsset {"Kind":"Shader","Guid":"203f89c9e5810dbffdb218d62826b69d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GIResolve"
{
    // One-bounce RSM indirect light resolved into its OWN buffer (rgb = indirect radiance), so the
    // deferred lighting pass can read it through a wide blur. The GIResolveRenderer then runs the shared
    // SSRResolve denoiser over this to accumulate it temporally.

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
        // GI resolve: computes the one-bounce RSM indirect light into its OWN buffer (rgb = indirect radiance)
        // so the lighting pass can read it through a wide blur. Computing this inline in DeferredLighting made
        // the per-pixel jittered VPL gather show as heavy grain — noise can only be filtered once it lives in a
        // texture. Variance is also reduced at the source: a distance floor kills the 1/d^2 fireflies that no
        // practical blur can hide.

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_GBufferB;    // rgb = world normal
        Uniform(2) sampler2D u_GBufferC;    // rgb = world position
        Uniform(3) sampler2D u_RSMAlbedo;   // rgb = surface albedo (flux colour)
        Uniform(4) sampler2D u_RSMNormal;   // rgb = surface world normal
        Uniform(5) sampler2D u_RSMWorldPos; // rgb = surface world position

        Out(0) vec4 oColor;

        Uniform(0) GIResolveUB
        {
        	mat4 u_RSMViewProj;  // world -> RSM clip (project the fragment into the sun's view)
        	vec4 u_SunColor;     // rgb = colour, a = intensity
        	vec4 u_Params;       // x = GI intensity, y = enabled (>0.5), z unused, w = per-frame jitter seed
        };

        float giHash(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        void main()
        {
        	vec3 N = texture(u_GBufferB, v_TexCoord).rgb;
        	if (u_Params.y < 0.5 || dot(N, N) <= 0.001) { oColor = vec4(0.0); return; } // off / sky
        	N = normalize(N);

        	vec3 worldPos = texture(u_GBufferC, v_TexCoord).rgb;
        	vec3 sunRadiance = u_SunColor.rgb * u_SunColor.a;

        	vec4 clip = u_RSMViewProj * vec4(worldPos, 1.0);
        	if (clip.w <= 0.0) { oColor = vec4(0.0); return; }
        	vec2 baseUV = clip.xy / clip.w * 0.5 + 0.5;
        	baseUV.y = 1.0 - baseUV.y; // RSM is rendered through the engine's Y-flipped viewport

        	const int   SAMPLES = 32;
        	const float RADIUS  = 0.10;      // RSM-space gather radius
        	const float GOLDEN  = 2.3999632; // golden angle for an even spiral

        	// Per-pixel rotation, DIFFERENT each frame (seed in Params.w) — the temporal accumulation pass
        	// averages a fresh estimate every frame, which is what actually converges the close-up VPL noise.
        	float ang = giHash(v_TexCoord * 4096.0 + vec2(u_Params.w)) * 6.2831853;

        	vec3 indirect = vec3(0.0);
        	for (int i = 0; i < SAMPLES; i++)
        	{
        		float r = RADIUS * sqrt((float(i) + 0.5) / float(SAMPLES));
        		float a = ang + float(i) * GOLDEN;
        		vec2  suv = baseUV + vec2(cos(a), sin(a)) * r;
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        		vec3 vplN = texture(u_RSMNormal, suv).rgb;
        		if (dot(vplN, vplN) <= 0.001) continue; // empty RSM texel (no caster)
        		vplN = normalize(vplN);

        		vec3 vplPos  = texture(u_RSMWorldPos, suv).rgb;
        		vec3 vplFlux = texture(u_RSMAlbedo, suv).rgb * sunRadiance;

        		vec3  dir = worldPos - vplPos;
        		// Distance FLOOR (not just an epsilon): a VPL almost coincident with the receiver (wall-floor
        		// corners) otherwise contributes ~1/eps — the single-sample fireflies that read as grain.
        		float d2  = max(dot(dir, dir), 0.25);
        		vec3  dn  = normalize(dir);
        		float recv = max(0.0, dot(N, -dn));   // this surface faces the VPL
        		float emit = max(0.0, dot(vplN, dn)); // the VPL faces this surface
        		indirect += vplFlux * recv * emit / d2;
        	}

        	oColor = vec4(indirect / float(SAMPLES) * u_Params.x, 1.0);
        }
    }
}

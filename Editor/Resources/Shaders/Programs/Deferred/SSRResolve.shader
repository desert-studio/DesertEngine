// DesertAsset {"Kind":"Shader","Guid":"427c3aa249fa0efeada3b23f641960fb","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRResolve"
{
    // Shared denoiser for the 1-sample-per-pixel jittered estimates: used by SSR (trace) and by the
    // RSM-GI gather. Spatial 5x5 alpha-weighted tent + AABB-clamped temporal accumulation.

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
        // SSR temporal + spatial resolve — the denoiser. The trace is a 1-sample-per-pixel jittered estimate
        // (deliberately different every frame); this pass:
        //  1) SPATIAL: 5x5 alpha-weighted tent over the trace (misses don't darken hits; holes get filled),
        //     also collecting the neighbourhood colour AABB.
        //  2) TEMPORAL: reprojects this pixel's WORLD position through LAST frame's camera and blends the
        //     previous resolved result in (exponential accumulation ~10 frames — this is what actually kills
        //     the HDR speckle). History is clamped to the current neighbourhood AABB so moving objects /
        //     disocclusions don't ghost.
        // Output goes to the accumulation target (ping-pong) and is what the composite pass blends on screen.

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Trace;           // this frame's jittered trace (rgb, a = reflectance)
        Uniform(2) sampler2D u_History;         // previous frame's resolved result
        Uniform(3) sampler2D u_GBufferWorldPos; // rgb = world position (for reprojection)

        Out(0) vec4 oColor;

        Uniform(0) SSRResolveUB
        {
        	mat4 u_PrevViewProj; // LAST frame's world -> clip
        	vec4 u_Params;       // xy = texel size, z = history blend (0 = first frame / resize), w unused
        };

        void main()
        {
        	vec2 t = u_Params.xy;

        	// --- Spatial: alpha-weighted 5x5 tent + neighbourhood AABB (for the temporal clamp). ---
        	vec3  colAcc = vec3(0.0);
        	float aAcc   = 0.0;
        	float wsum   = 0.0;
        	vec3  mnC = vec3(1e9), mxC = vec3(-1e9);
        	float mnA = 1e9, mxA = -1e9;
        	for (int y = -2; y <= 2; y++)
        		for (int x = -2; x <= 2; x++)
        		{
        			float w  = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
        			vec2  uv = clamp(v_TexCoord + vec2(x, y) * t, vec2(0.001), vec2(0.999));
        			vec4  s  = texture(u_Trace, uv);
        			colAcc += s.rgb * s.a * w;
        			aAcc   += s.a * w;
        			wsum   += w;

        			vec3 c = s.a > 0.001 ? s.rgb : vec3(0.0);
        			mnC = min(mnC, c);
        			mxC = max(mxC, c);
        			mnA = min(mnA, s.a);
        			mxA = max(mxA, s.a);
        		}

        	vec3  curCol = aAcc > 0.001 ? colAcc / aAcc : vec3(0.0);
        	float curA   = aAcc / wsum;
        	vec4  cur    = vec4(curCol, curA);

        	// --- Temporal: reproject via last frame's camera (exact for static geometry), AABB-clamped. ---
        	float blend = u_Params.z;
        	if (blend > 0.0)
        	{
        		vec3 wp = texture(u_GBufferWorldPos, v_TexCoord).rgb;
        		vec4 pc = u_PrevViewProj * vec4(wp, 1.0);
        		if (pc.w > 0.0)
        		{
        			vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
        			puv.y = 1.0 - puv.y; // engine Y-flipped viewport (same convention as the trace)
        			if (puv.x > 0.0 && puv.x < 1.0 && puv.y > 0.0 && puv.y < 1.0)
        			{
        				vec4 hist = texture(u_History, puv);
        				hist.rgb  = clamp(hist.rgb, mnC, mxC);
        				hist.a    = clamp(hist.a, mnA, mxA);
        				cur = mix(cur, hist, blend);
        			}
        		}
        	}

        	oColor = cur;
        }
    }
}

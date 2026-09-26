// DesertAsset {"Kind":"Shader","Guid":"c9a95e12430cebced5d5e4c735cb68ed","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSAO"
{
    // Screen-space ambient occlusion (fullscreen). Reads the G-buffer world position + normal, writes a
    // single-channel AO factor the deferred lighting pass multiplies into the ambient term.

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
        // Screen-space ambient occlusion (SSAO). Reads the G-buffer world position + world normal, samples a
        // hemisphere of points around each fragment, projects them back to screen space and counts how many are
        // occluded by nearer geometry -> an ambient-occlusion factor in [0,1] (1 = fully lit, 0 = fully occluded).
        // Self-contained: hemisphere directions come from a Hammersley sequence, rotated per-pixel by a hash (no
        // noise texture). The deferred lighting pass multiplies the ambient term by this.

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_GBufferPos;    // rgb = world position
        Uniform(2) sampler2D u_GBufferNormal; // rgb = world normal

        Out(0) vec4 oAO;

        Uniform(0) SSAOUB
        {
        	mat4 u_ViewProj;    // world -> clip (to project sample points back to screen)
        	vec4 u_CameraPos;   // xyz = camera world position
        	vec4 u_SSAOParams;  // x = radius, y = bias, z = power, w = sample count
        };

        const uint MAX_SAMPLES = 32u;

        // Van der Corput / Hammersley low-discrepancy 2D point.
        vec2 hammersley(uint i, uint n)
        {
        	uint bits = i;
        	bits = (bits << 16u) | (bits >> 16u);
        	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        	return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
        }

        // Cosine-weighted hemisphere direction around +Z.
        vec3 hemisphereDir(vec2 u)
        {
        	float r   = sqrt(u.x);
        	float phi = 6.2831853 * u.y;
        	return vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
        }

        float hash12(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        void main()
        {
        	vec3 normal = texture(u_GBufferNormal, v_TexCoord).rgb;
        	// No geometry (G-buffer cleared to 0): fully lit, no occlusion.
        	if (dot(normal, normal) <= 0.001)
        	{
        		oAO = vec4(1.0);
        		return;
        	}

        	vec3 worldPos = texture(u_GBufferPos, v_TexCoord).rgb;
        	vec3 N        = normalize(normal);

        	float radius = u_SSAOParams.x;
        	float bias   = u_SSAOParams.y;
        	float power  = u_SSAOParams.z;
        	uint  count  = uint(u_SSAOParams.w + 0.5);
        	if (count > MAX_SAMPLES) count = MAX_SAMPLES;

        	// Per-pixel rotated tangent basis (breaks up banding without a noise texture).
        	float ang = hash12(v_TexCoord * 4096.0) * 6.2831853;
        	vec3  rvec = vec3(cos(ang), sin(ang), 0.0);
        	vec3  T    = normalize(rvec - N * dot(rvec, N));
        	vec3  B    = cross(N, T);
        	mat3  TBN  = mat3(T, B, N);

        	float camDistFrag = distance(u_CameraPos.xyz, worldPos);

        	float occlusion = 0.0;
        	for (uint i = 0u; i < count; i++)
        	{
        		vec3 dir       = hemisphereDir(hammersley(i, count));
        		// Weight samples toward the origin for a tighter, contact-focused AO.
        		float scale    = mix(0.1, 1.0, float(i) / float(count));
        		vec3  samplePos = worldPos + (TBN * dir) * radius * scale;

        		vec4 clip = u_ViewProj * vec4(samplePos, 1.0);
        		if (clip.w <= 0.0) continue;
        		vec3 ndc = clip.xyz / clip.w;
        		vec2 suv = ndc.xy * 0.5 + 0.5;
        		suv.y = 1.0 - suv.y; // engine renders through a Y-flipped viewport
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        		vec3 sampledNormal = texture(u_GBufferNormal, suv).rgb;
        		if (dot(sampledNormal, sampledNormal) <= 0.001) continue; // sky -> not an occluder

        		vec3  sampledPos   = texture(u_GBufferPos, suv).rgb;
        		float camDistSurf  = distance(u_CameraPos.xyz, sampledPos);
        		float camDistSample= distance(u_CameraPos.xyz, samplePos);

        		// The real surface at this screen texel is nearer the camera than our sample point -> it occludes.
        		// Range check discards occluders far from the fragment (avoids dark halos across depth gaps).
        		float rangeCheck = smoothstep(0.0, 1.0, radius / max(0.0001, abs(camDistFrag - camDistSurf)));
        		if (camDistSurf < camDistSample - bias)
        			occlusion += rangeCheck;
        	}

        	float ao = 1.0 - occlusion / float(count);
        	oAO = vec4(vec3(pow(clamp(ao, 0.0, 1.0), power)), 1.0);
        }
    }
}

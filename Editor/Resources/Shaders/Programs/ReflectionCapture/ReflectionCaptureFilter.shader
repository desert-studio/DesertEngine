// DesertAsset {"Kind":"Shader","Guid":"a5b85f97a09c44e9ad3303e32772224c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureFilter"
{
    Compute
    {
        // ONE MIP OF ONE CAPTURE IN THE CAPTURE ARRAY: the capture's radiance cube GGX-prefiltered at this mip's
        // roughness, the same convolution and the same roughness ramp (Common/ReflectionCaptureMip.glslh, UE's
        // ComputeReflectionCaptureRoughnessFromMip) as the sky's prefiltered cube (PrefilterEnvMap.shader), so a
        // capture and the sky it blends into are read with one LOD rule.
        //
        // ALPHA IS THE CAPTURE'S AVERAGE BRIGHTNESS (UE: FReflectionCaptureData::AverageBrightness, computed from
        // the captured cube): the luminance of its mean radiance, read from the radiance cube's 1x1 mip. Stored in
        // every texel of the capture, so the lighting pass reads it with the very sample it reads the radiance
        // with and the normalisation (UE: ComputeMixingWeight) costs no extra fetch and no extra buffer.

        const float PI = 3.141592;
        const float TwoPI = 2 * PI;
        const float Epsilon = 0.00001;
        const uint NumSamples = 1024;
        const float InvNumSamples = 1.0 / float(NumSamples);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(2) sampler u_LinearClampSampler;
        Uniform(0) textureCube u_Radiance; // every mip of the capture's radiance cube
        #define u_Radiance samplerCube(u_Radiance, u_LinearClampSampler)
        layout(binding = 1, rgba16f) writeonly uniform imageCubeArray u_Output; // this mip, every layer

        PushConstant PushConstants
        {
        	uint u_Mip;        // the mip this dispatch writes
        	uint u_MaxMip;     // the array's last mip index
        	uint u_FirstLayer; // the capture's first layer: cube index * 6
        };

        #include <Common/ReflectionCaptureMip.glslh>

        float radicalInverse_VdC(uint bits)
        {
        	bits = (bits << 16u) | (bits >> 16u);
        	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        	return float(bits) * 2.3283064365386963e-10;
        }

        vec2 sampleHammersley(uint i)
        {
        	return vec2(i * InvNumSamples, radicalInverse_VdC(i));
        }

        vec3 sampleGGX(float u1, float u2, float roughness)
        {
        	float alpha = roughness * roughness;
        	float cosTheta = sqrt((1.0 - u2) / (1.0 + (alpha*alpha - 1.0) * u2));
        	float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
        	float phi = TwoPI * u1;
        	return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
        }

        float ndfGGX(float cosLh, float roughness)
        {
        	float alpha   = roughness * roughness;
        	float alphaSq = alpha * alpha;
        	float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
        	return alphaSq / (PI * denom * denom);
        }

        vec3 CubeTexelDirection(uvec3 id, vec2 size)
        {
        	vec2 st = (vec2(id.xy) + 0.5) / size;
        	vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);
        	vec3 ret;
        	if (id.z == 0)      ret = vec3(1.0,  uv.y, -uv.x);
        	else if (id.z == 1) ret = vec3(-1.0, uv.y,  uv.x);
        	else if (id.z == 2) ret = vec3(uv.x, 1.0, -uv.y);
        	else if (id.z == 3) ret = vec3(uv.x, -1.0, uv.y);
        	else if (id.z == 4) ret = vec3(uv.x, uv.y, 1.0);
        	else                ret = vec3(-uv.x, uv.y, -1.0);
        	return normalize(ret);
        }

        void computeBasisVectors(const vec3 N, out vec3 S, out vec3 T)
        {
        	T = cross(N, vec3(0.0, 1.0, 0.0));
        	T = mix(cross(N, vec3(1.0, 0.0, 0.0)), T, step(Epsilon, dot(T, T)));
        	T = normalize(T);
        	S = normalize(cross(N, T));
        }

        float Luminance(vec3 c)
        {
        	return dot(c, vec3(0.2126, 0.7152, 0.0722));
        }

        // The mean radiance of the capture: the six texels of the radiance cube's 1x1 mip, each the mean of its
        // face (equal solid angles to within the cube's own distortion, which the box chain already ignores).
        float AverageBrightness()
        {
        	const float top = float(textureQueryLevels(u_Radiance) - 1);
        	vec3 sum = textureLod(u_Radiance, vec3( 1, 0, 0), top).rgb + textureLod(u_Radiance, vec3(-1, 0, 0), top).rgb +
        	           textureLod(u_Radiance, vec3( 0, 1, 0), top).rgb + textureLod(u_Radiance, vec3( 0,-1, 0), top).rgb +
        	           textureLod(u_Radiance, vec3( 0, 0, 1), top).rgb + textureLod(u_Radiance, vec3( 0, 0,-1), top).rgb;
        	return Luminance(sum / 6.0);
        }

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size = imageSize(u_Output).xy;
        	if (gl_GlobalInvocationID.x >= uint(size.x) || gl_GlobalInvocationID.y >= uint(size.y))
        		return;

        	const ivec3 texel = ivec3(gl_GlobalInvocationID.xy, int(u_FirstLayer + gl_GlobalInvocationID.z));
        	const float brightness = AverageBrightness();
        	const vec3  N = CubeTexelDirection(gl_GlobalInvocationID, vec2(size));
        	const float roughness = ComputeReflectionCaptureRoughnessFromMip(float(u_Mip), float(u_MaxMip));

        	// Roughness 0 is the radiance itself (see PrefilterEnvMap: the GGX path is 0/0 there); a limit guard.
        	if (roughness <= Epsilon)
        	{
        		imageStore(u_Output, texel, vec4(textureLod(u_Radiance, N, 0.0).rgb, brightness));
        		return;
        	}

        	const vec2  inputSize = vec2(textureSize(u_Radiance, 0));
        	const float wt = 4.0 * PI / (6 * inputSize.x * inputSize.y);
        	vec3 S, T;
        	computeBasisVectors(N, S, T);
        	vec3  color  = vec3(0);
        	float weight = 0;
        	for (uint i = 0; i < NumSamples; i++)
        	{
        		vec2 u  = sampleHammersley(i);
        		vec3 h  = sampleGGX(u.x, u.y, roughness);
        		vec3 Lh = S * h.x + T * h.y + N * h.z;
        		vec3 Li = 2.0 * dot(N, Lh) * Lh - N;
        		float cosLi = dot(N, Li);
        		if (cosLi > 0.0)
        		{
        			float cosLh = max(dot(N, Lh), 0.0);
        			float pdf = ndfGGX(cosLh, roughness) * 0.25;
        			float ws = 1.0 / (NumSamples * pdf);
        			float mipLevel = max(0.5 * log2(ws / wt) + 1.0, 0.0);
        			color  += textureLod(u_Radiance, Li, mipLevel).rgb * cosLi;
        			weight += cosLi;
        		}
        	}
        	imageStore(u_Output, texel, vec4(color / max(weight, Epsilon), brightness));
        }
    }
}

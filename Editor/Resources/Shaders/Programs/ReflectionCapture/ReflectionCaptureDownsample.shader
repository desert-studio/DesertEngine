// DesertAsset {"Kind":"Shader","Guid":"6901eb275309400fb10867534a8ca32e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureDownsample"
{
    Compute
    {
        // The radiance cube's mip chain, one mip from the one above: each output texel averages the four texels
        // of the source mip it covers (bilinear taps at the four sub-texel centres). The chain is what the GGX
        // filter's filtered importance sampling reads (ReflectionCaptureFilter: a wide lobe samples a coarse mip),
        // exactly as the sky's prefilter reads its radiance cube's chain.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(2) sampler LinearClampSampler;
        Uniform(0) textureCube SourceCubeTexture; // the source mip only (a single-mip view)
        #define SourceCubeTexture samplerCube(SourceCubeTexture, LinearClampSampler)
        layout(binding = 1, rgba16f) writeonly uniform imageCube DestinationCubeTexture;

        vec3 CubeDirection(vec2 texel, uint face, vec2 size)
        {
        	vec2 st = texel / size;
        	vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);
        	vec3 ret;
        	if (face == 0)      ret = vec3(1.0,  uv.y, -uv.x);
        	else if (face == 1) ret = vec3(-1.0, uv.y,  uv.x);
        	else if (face == 2) ret = vec3(uv.x, 1.0, -uv.y);
        	else if (face == 3) ret = vec3(uv.x, -1.0, uv.y);
        	else if (face == 4) ret = vec3(uv.x, uv.y, 1.0);
        	else                ret = vec3(-uv.x, uv.y, -1.0);
        	return normalize(ret);
        }

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size = imageSize(DestinationCubeTexture);
        	if (gl_GlobalInvocationID.x >= uint(size.x) || gl_GlobalInvocationID.y >= uint(size.y))
        		return;
        	const vec2 base = vec2(gl_GlobalInvocationID.xy);
        	const uint face = gl_GlobalInvocationID.z;
        	vec3 sum = vec3(0.0);
        	for (int j = 0; j < 2; ++j)
        		for (int i = 0; i < 2; ++i)
        			sum += textureLod(SourceCubeTexture, CubeDirection(base + vec2(0.25 + 0.5 * i, 0.25 + 0.5 * j), face,
        			                                          vec2(size)), 0.0).rgb;
        	imageStore(DestinationCubeTexture, ivec3(gl_GlobalInvocationID), vec4(sum * 0.25, 1.0));
        }
    }
}

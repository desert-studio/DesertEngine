// DesertAsset {"Kind":"Shader","Guid":"27389d6f3d254372991450246aa27aa1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyCaptureDownsample"
{
    Compute
    {
        // The SKY CAPTURE's radiance mip chain (Systems/Scene/Skybox/SkyCapture.cpp), one mip from the one above:
        // ReflectionCaptureDownsample's 2x2 box, for a cube of another format. A storage image's format qualifier
        // must be the image's own: the sky's radiance cube is RGBA32F (the format the sky's IBL chain has always been
        // built in, PanoramaToCubemap's output), the reflection captures' RGBA16F.

        Uniform sampler LinearClampSampler;
        Uniform(0) textureCube SourceCubeMipTexture; // the source mip only (a single-mip view)
        #define SourceCubeMipTexture samplerCube(SourceCubeMipTexture, LinearClampSampler)
        layout(binding = 1, rgba32f) writeonly uniform imageCube DestinationCubeTexture;

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
        			sum += textureLod(SourceCubeMipTexture, CubeDirection(base + vec2(0.25 + 0.5 * i, 0.25 + 0.5 * j), face,
        			                                          vec2(size)), 0.0).rgb;
        	imageStore(DestinationCubeTexture, ivec3(gl_GlobalInvocationID), vec4(sum * 0.25, 1.0));
        }
    }
}

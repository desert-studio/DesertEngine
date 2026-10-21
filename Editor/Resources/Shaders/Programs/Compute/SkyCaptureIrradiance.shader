// DesertAsset {"Kind":"Shader","Guid":"bb12b699b66d44528f785aa0faaab93e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyCaptureIrradiance"
{
    Compute
    {
        // THE SKY CAPTURE'S DIFFUSE IRRADIANCE (Systems/Scene/Skybox/SkyCapture.cpp): every texel of the irradiance
        // cube is the cosine-weighted integral of the radiance cube over its hemisphere, summed EXACTLY over every
        // texel of one coarse mip of that cube -- each texel weighted by its own solid angle.
        //
        // WHY NOT DiffuseIrradiance's Monte Carlo: 65 536 samples per output texel is 400 M samples for a 32-face
        // cube, measured at 355 ms of GPU on an M1 Pro -- a capture recorded in the frame cannot pay that. The
        // integrand is the smoothest one in the renderer (the cosine lobe spans a hemisphere), so a 16-face mip
        // of the radiance cube, box-filtered from the full one and therefore holding the sky's whole energy,
        // carries all of it: 1 536 texels per output texel, 9.4 M taps in all, and no sampling noise at all.
        // UE projects the same capture onto third-order SH (ReflectionEnvironmentShaders.usf DiffuseIrradiance*);
        // the exact sum costs the same class of time without SH's ringing around the sun, and its consumers keep
        // reading an irradiance cube.
        //
        // The value stored is DiffuseIrradiance's: E / pi, the exitant radiance of a white Lambertian surface.

        const float PI = 3.141592653589793;

        Uniform(0, 2) sampler u_PointClampSampler; // ComputeImages' kComputeImagesSamplerBinding
        Uniform(0) textureCube u_Radiance; // the radiance cube's coarse mip only (a single-mip view)
        #define u_Radiance samplerCube(u_Radiance, u_PointClampSampler)
        layout(binding = 1, rgba32f) writeonly uniform imageCube u_Output;

        // The direction of the face texel at (st in [0,1]^2), the convention PanoramaToCubemap writes the cube in.
        vec3 CubeDirection(vec2 st, uint face)
        {
        	vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);
        	vec3 ret;
        	if (face == 0u)      ret = vec3(1.0,  uv.y, -uv.x);
        	else if (face == 1u) ret = vec3(-1.0, uv.y,  uv.x);
        	else if (face == 2u) ret = vec3(uv.x, 1.0, -uv.y);
        	else if (face == 3u) ret = vec3(uv.x, -1.0, uv.y);
        	else if (face == 4u) ret = vec3(uv.x, uv.y, 1.0);
        	else                 ret = vec3(-uv.x, uv.y, -1.0);
        	return normalize(ret);
        }

        // The exact solid angle of the face texel spanning [x0,x1] x [y0,y1] of the unit cube face's [-1,1]^2
        // (the area element integrated in closed form: atan(xy / sqrt(x^2 + y^2 + 1)) at the four corners).
        float CornerArea(float x, float y)
        {
        	return atan(x * y, sqrt(x * x + y * y + 1.0));
        }
        float TexelSolidAngle(vec2 lo, vec2 hi)
        {
        	return CornerArea(lo.x, lo.y) - CornerArea(lo.x, hi.y) - CornerArea(hi.x, lo.y) + CornerArea(hi.x, hi.y);
        }

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size = imageSize(u_Output);
        	if (gl_GlobalInvocationID.x >= uint(size.x) || gl_GlobalInvocationID.y >= uint(size.y))
        		return;
        	const vec3 N = CubeDirection((vec2(gl_GlobalInvocationID.xy) + 0.5) / vec2(size), gl_GlobalInvocationID.z);

        	const int  source = textureSize(u_Radiance, 0).x;
        	const float texel = 2.0 / float(source);
        	vec3 sum = vec3(0.0);
        	for (uint face = 0u; face < 6u; ++face)
        		for (int y = 0; y < source; ++y)
        			for (int x = 0; x < source; ++x)
        			{
        				const vec2  st  = (vec2(x, y) + 0.5) / float(source);
        				const vec3  L   = CubeDirection(st, face);
        				const float cosine = dot(N, L);
        				if (cosine <= 0.0)
        					continue;
        				const vec2  lo    = vec2(-1.0) + vec2(x, y) * texel;
        				const float omega = TexelSolidAngle(lo, lo + vec2(texel));
        				sum += textureLod(u_Radiance, L, 0.0).rgb * (cosine * omega);
        			}
        	imageStore(u_Output, ivec3(gl_GlobalInvocationID), vec4(sum / PI, 1.0));
        }
    }
}

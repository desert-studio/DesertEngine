Shader "DiffuseIrradiance"
{
    Compute
    {
        const float PI = 3.14159265359;
        const float TWOPI = 2.0 * PI;
        const float Epsilon = 0.00001;

        const uint NumSamples = 64 * 1024;
        const float InvNumSamples = 1.0 / float(NumSamples);

        Uniform(0) sampler2D  inputTexture;
        layout(binding=1, rgba32f) restrict writeonly uniform imageCube outputTexture;

        // NO LOOK HERE, for the same reason as PanoramaToCubemap: the scene's rotation and gain are
        // applied where this cube is sampled (Common/SkyLook.glslh), so a rotation never re-integrates.
        #include <Common/SkyPanorama.glslh>

        float radicalInverse_VdC(uint bits)
        {
        	bits = (bits << 16u) | (bits >> 16u);
        	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        	return float(bits) * 2.3283064365386963e-10; // / 0x100000000
        }

        // Sample i-th point from Hammersley point set of NumSamples points total.
        vec2 sampleHammersley(uint i)
        {
        	return vec2(i * InvNumSamples, radicalInverse_VdC(i));
        }

        // Uniformly sample point on a hemisphere.
        // Cosine-weighted sampling would be a better fit for Lambertian BRDF but since this
        // compute shader runs only once as a pre-processing step performance is not *that* important.
        // See: "Physically Based Rendering" 2nd ed., section 13.6.1.
        vec3 sampleHemisphere(float u1, float u2)
        {
        	const float u1p = sqrt(max(0.0, 1.0 - u1*u1));
        	return vec3(cos(TWOPI*u2) * u1p, sin(TWOPI*u2) * u1p, u1);
        }

        // Calculate normalized sampling direction vector based on current fragment coordinates (gl_GlobalInvocationID.xyz).
        // This is essentially "inverse-sampling": we reconstruct what the sampling vector would be if we wanted it to "hit"
        // this particular fragment in a cubemap.
        // See: OpenGL core profile specs, section 8.13.
        vec3 getSamplingVector()
        {
            vec2 st = gl_GlobalInvocationID.xy/vec2(imageSize(outputTexture));
            vec2 uv = 2.0 * vec2(st.x, 1.0-st.y) - vec2(1.0);

            vec3 ret;
            // Sadly 'switch' doesn't seem to work, at least on NVIDIA.
            if(gl_GlobalInvocationID.z == 0)      ret = vec3(1.0,  uv.y, -uv.x);
            else if(gl_GlobalInvocationID.z == 1) ret = vec3(-1.0, uv.y,  uv.x);
            else if(gl_GlobalInvocationID.z == 2) ret = vec3(uv.x, 1.0, -uv.y);
            else if(gl_GlobalInvocationID.z == 3) ret = vec3(uv.x, -1.0, uv.y);
            else if(gl_GlobalInvocationID.z == 4) ret = vec3(uv.x, uv.y, 1.0);
            else if(gl_GlobalInvocationID.z == 5) ret = vec3(-uv.x, uv.y, -1.0);
            return normalize(ret);
        }

        // Compute orthonormal basis for converting from tanget/shading space to world space.
        void computeBasisVectors(const vec3 N, out vec3 S, out vec3 T)
        {
        	// Branchless select non-degenerate T.
        	T = cross(N, vec3(0.0, 1.0, 0.0));
        	T = mix(cross(N, vec3(1.0, 0.0, 0.0)), T, step(Epsilon, dot(T, T)));

        	T = normalize(T);
        	S = normalize(cross(N, T));
        }

        // Convert point from tangent/shading space to world space.
        vec3 tangentToWorld(const vec3 v, const vec3 N, const vec3 S, const vec3 T)
        {
        	return S * v.x + T * v.y + N * v.z;
        }

        LocalSize(32, 32, 1);
        void main(void)
        {
        	// An invocation outside the face has nothing to write, and each one of them would otherwise run
        	// the full 65536-sample integral before an out-of-range imageStore discarded it. The dispatch is
        	// rounded up to whole workgroups, so this is what makes that rounding free.
        	if (any(greaterThanEqual(ivec2(gl_GlobalInvocationID.xy), imageSize(outputTexture))))
        		return;

        	vec3 N = getSamplingVector();

        	vec3 S, T;
        	computeBasisVectors(N, S, T);

        	// Monte Carlo integration of hemispherical irradiance.
        	// As a small optimization this also includes Lambertian BRDF assuming perfectly white surface (albedo of 1.0)
        	// so we don't need to normalize in PBR fragment shader (so technically it encodes exitant radiance rather than irradiance).
        	//
        	// MIPMAP-FILTERED IMPORTANCE SAMPLING (Colbert & Krivanek, GPU Gems 3 ch. 20.4 -- the same rule
        	// PrefilterEnvMap applies to the radiance cube). Each sample stands for 2*pi/NumSamples sr of the
        	// hemisphere, so it reads the panorama level whose texel covers that much sky, instead of one
        	// level-0 texel. Point-sampling level 0 is what made the matte sphere under
        	// rural_asphalt_road_2k.hdr blotchy: its sun is nine texels at up to 131072, a sample spacing of
        	// ~3 level-0 texels either hits one of them or misses all, and which it does changes from one
        	// irradiance texel to the next. A panorama with ONE level (the procedural bake's) clamps every
        	// LOD to 0, so that path integrates exactly what it integrated before.
        	vec2  panoramaSize = vec2(textureSize(inputTexture, 0));
        	float panoramaTop  = float(textureQueryLevels(inputTexture) - 1);
        	// Solid angle of one uniform-hemisphere sample, and of one level-0 texel on the equator (an
        	// equirect texel shrinks by sin(theta) towards the poles, applied per sample below).
        	const float sampleSolidAngle = TWOPI / float(NumSamples);
        	float       texelSolidAngle  = (TWOPI / panoramaSize.x) * (PI / panoramaSize.y);

        	vec3 irradiance = vec3(0);
        	for(uint i=0; i<NumSamples; ++i) {
        		vec2 u  = sampleHammersley(i);
        		vec3 Li = tangentToWorld(sampleHemisphere(u.x, u.y), N, S, T);
        		float cosTheta = max(0.0, dot(Li, N));

        		// Shared with PanoramaToCubemap — see Common/SkyPanorama.glslh for why the mapping may
        		// not exist twice.
        		vec2 sampleUV = PanoramaSampleUV(Li);

        		// PIs here cancel out because of division by pdf.
        		// The +1 is Colbert & Krivanek's bias: the footprint of a sample overlaps its neighbours',
        		// which is what makes the sum smooth rather than merely unbiased. The pole guard keeps the
        		// log finite where an equirect texel's solid angle goes to zero.
        		float sinTheta = sqrt(max(1.0 - Li.y * Li.y, 1e-6));
        		float lod      = clamp(0.5 * log2(sampleSolidAngle / (texelSolidAngle * sinTheta)) + 1.0,
        		                       0.0, panoramaTop);

        		irradiance += 2.0 * textureLod(inputTexture, sampleUV, lod).rgb * cosTheta;
        	}
        	irradiance /= vec3(NumSamples);

        	imageStore(outputTexture, ivec3(gl_GlobalInvocationID), vec4(irradiance, 1.0));
        }
    }
}

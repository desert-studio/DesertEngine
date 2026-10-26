// DesertAsset {"Kind":"Shader","Guid":"5b1f0c7e9a2d4e638f41c0d2a7b3e915","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SubsurfaceBurley"
{
    // "Subsurface: Burley" (SSS-PROFILE, CONTRACT §5.3): the screen-space Burley normalized diffusion of the
    // composite's SubsurfaceDiffuse - UE PostProcessSubsurface.usf MainIndirectDispatchCS + SubsurfaceBurleyNormalized.ush
    // BurleyNormalizedSS, the importance-sampled (AFIS) form at a fixed sample count. Each scattered texel draws its
    // samples on the TANGENT PLANE in world space (radius importance-sampled from the profile's widest channel),
    // projects them, and weights each by the profile of the bilateral radius sqrt(r^2 + off-plane^2) over the pdf
    // it was drawn with - so a depth step (the nose over the cheek) contributes little, and a sample on another
    // profile, an unscattered surface or the sky contributes nothing (no halo onto the background). The weights are
    // normalized per channel (UE: the sum of weights), so a texel keeps its energy wherever its samples land.
    // THE HISTORY (UE PostProcessSubsurface SubsurfaceHistory, r.SSS.Burley.Quality 1): the sample disc's rotation
    // differs per pixel and per frame, and the temporal AA does not integrate that noise - its neighbourhood clamp
    // keeps a pixel's own outlier inside the box, so the dots survive any number of frames. The scatter therefore
    // keeps its own accumulation: each texel reprojects its world point into the previous frame, takes that
    // texel's running mean when it is the same surface (same profile, on this texel's tangent plane), and blends the
    // new estimate in at 1/(n+1) up to the most frames; the running second moment of the luminance gives the
    // variance, which picks this frame's sample count (UE's variance-adaptive count): a converged texel draws fewer.
    Compute
    {
        #include <Common/ReconstructPosition.glslh>
        #include <Common/SubsurfaceProfileCommon.glslh>

        Uniform sampler PointClampSampler;
        Uniform(0) texture2D SubsurfaceDiffuseTexture; // rgb = demodulated diffuse (pre-exposed), a = profile row + 1
        #define SubsurfaceDiffuseTexture sampler2D(SubsurfaceDiffuseTexture, PointClampSampler)
        Uniform(1) texture2D SceneDepthTexture;        // the G-buffer's device depth
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        Uniform(2) texture2D GBufferBTexture;          // rgb = world normal
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        ReadBuffer(3) SubsurfaceProfileAtlasUB
        {
        	vec4 profileAtlas[]; // SSSS_ATLAS_WIDTH columns per profile row (Common/SubsurfaceProfileLayout.glslh)
        };
        ReadBuffer(4) SubsurfaceViewUB
        {
        	mat4 WorldToClip;  // the jittered view-projection the G-buffer was rasterised with
        	mat4 ClipToWorld;  // its inverse
        	vec4 ViewParams;   // x = PostProcessSettings SubsurfaceScale, y = most samples, z = frame index, w = fewest
        	mat4 PrevWorldToClip; // the previous frame's jittered view-projection (the history's raster)
        	mat4 PrevClipToWorld; // its inverse
        	vec4 HistoryParams;   // x = history readable (1/0), y = PreExposure / PrevPreExposure, z = most frames, w unused
        };
        layout(binding = 5, rgba16f) writeonly uniform image2D SubsurfaceScatteredTexture;
        Uniform(6) texture2D SubsurfaceHistoryTexture;        // previous frame: rgb = running mean, a = frames in it
        #define SubsurfaceHistoryTexture sampler2D(SubsurfaceHistoryTexture, PointClampSampler)
        Uniform(7) texture2D SubsurfaceHistoryMomentsTexture; // r = mean luminance^2, g = profile row + 1, b = device depth, a unused
        #define SubsurfaceHistoryMomentsTexture sampler2D(SubsurfaceHistoryMomentsTexture, PointClampSampler)
        layout(binding = 8, rgba16f) writeonly uniform image2D SubsurfaceHistoryOutTexture;
        layout(binding = 9, rgba32f) writeonly uniform image2D SubsurfaceHistoryMomentsOutTexture;

        float SubsurfaceLuminance(vec3 c)
        {
        	return dot(c, vec3(0.2126, 0.7152, 0.0722));
        }

        // Interleaved gradient noise (Jimenez 2014), rotated per frame: the temporal AA integrates the rotations.
        float SubsurfaceNoise(vec2 pixel, float frame)
        {
        	pixel += 5.588238 * mod(frame, 64.0);
        	return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
        }

        vec2 NdcToUV(vec2 ndc)
        {
        	// ScreenUVToNdc is affine per axis: invert it from its two corners (no y-flip convention assumed here).
        	const vec2 n0 = ScreenUVToNdc(vec2(0.0));
        	const vec2 n1 = ScreenUVToNdc(vec2(1.0));
        	return (ndc - n0) / (n1 - n0);
        }

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size  = imageSize(SubsurfaceScatteredTexture);
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const vec4 centre = texelFetch(SubsurfaceDiffuseTexture, pixel, 0);
        	const float depth = texelFetch(SceneDepthTexture, pixel, 0).r;
        	if (centre.a < 0.5 || depth <= 0.0)
        	{
        		imageStore(SubsurfaceScatteredTexture, pixel, vec4(0.0));
        		imageStore(SubsurfaceHistoryOutTexture, pixel, vec4(0.0));
        		imageStore(SubsurfaceHistoryMomentsOutTexture, pixel, vec4(0.0));
        		return;
        	}
        	const uint row    = (uint(centre.a + 0.5) - 1u) * SSSS_ATLAS_WIDTH;
        	const vec3 albedo = profileAtlas[row + SSSS_SURFACE_ALBEDO].rgb;
        	// The atlas holds the DIFFUSE mean free path (Graphic::DiffuseMeanFreePathFromMeanFreePath, UE
        	// GetDMFPFromMFPApprox); UE CalculateCenterSampleWeight: d = DMFP / s_searchlight(A).
        	const vec3 dmfp   = profileAtlas[row + SSSS_DIFFUSE_MEAN_FREE_PATH].rgb * ViewParams.x;
        	const vec3 d      = max(dmfp / BurleyScalingFactor(albedo), vec3(1e-4));
        	const float dMax  = max(d.r, max(d.g, d.b));
        	if (dMax <= 1e-3)
        	{
        		imageStore(SubsurfaceScatteredTexture, pixel, vec4(centre.rgb, 1.0));
        		imageStore(SubsurfaceHistoryOutTexture, pixel, vec4(0.0));
        		imageStore(SubsurfaceHistoryMomentsOutTexture, pixel, vec4(0.0));
        		return;
        	}

        	const vec2 texel = 1.0 / vec2(size);
        	const vec2 uv    = (vec2(pixel) + 0.5) * texel;
        	const vec3 P     = ReconstructWorldPosition(uv, depth, ClipToWorld);
        	const vec3 N     = normalize(texelFetch(GBufferBTexture, pixel, 0).rgb);
        	const vec3 up    = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        	const vec3 T     = normalize(cross(up, N));
        	const vec3 B     = cross(N, T);

        	// The previous frame's texel this surface point was in, when it is the same surface.
        	const float exposure = HistoryParams.y;
        	float frames = 0.0;
        	vec3  mean   = vec3(0.0);
        	float moment = 0.0;
        	if (HistoryParams.x > 0.5)
        	{
        		const vec4 prevClip = PrevWorldToClip * vec4(P, 1.0);
        		const vec2 prevUV   = prevClip.w > 0.0 ? NdcToUV(prevClip.xy / prevClip.w) : vec2(-1.0);
        		if (all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThan(prevUV, vec2(1.0))))
        		{
        			const ivec2 at          = ivec2(prevUV * vec2(size));
        			const vec4  history     = texelFetch(SubsurfaceHistoryTexture, at, 0);
        			const vec4  moments     = texelFetch(SubsurfaceHistoryMomentsTexture, at, 0);
        			const bool  sameProfile = history.a > 0.5 && abs(moments.g - centre.a) < 0.25;
        			// The surface the history texel saw, against this texel's tangent plane: a disocclusion (the
        			// nose uncovering the cheek) lies off it, a steep wall seen a jitter step aside does not.
        			const vec2 historyUV = (vec2(at) + 0.5) / vec2(size);
        			const vec3 seenWorld = ReconstructWorldPosition(historyUV, moments.b, PrevClipToWorld);
        			// clip w = the view depth: 2 % of it, ~0.4 cm at the 20 cm close-up.
        			const bool samePlane = abs(dot(N, seenWorld - P)) <= 0.02 * prevClip.w;
        			if (sameProfile && samePlane)
        			{
        				frames = history.a;
        				mean   = history.rgb * exposure;
        				moment = moments.r * exposure * exposure;
        			}
        		}
        	}
        	// Variance-adaptive sample count: the running relative deviation of the luminance against the deviation the
        	// accumulated mean may keep; a fresh texel draws the most.
        	int samples = max(int(ViewParams.y + 0.5), 1);
        	if (frames >= 4.0)
        	{
        		const float meanLum  = SubsurfaceLuminance(mean);
        		const float relative = sqrt(max(moment - meanLum * meanLum, 0.0)) / max(meanLum, 1e-6);
        		const float fewest   = max(ViewParams.w, 1.0);
        		samples = int(ceil(clamp(ViewParams.y * relative / 0.05, fewest, ViewParams.y)));
        	}
        	const float rotation = SubsurfaceNoise(vec2(pixel), ViewParams.z) * 6.28318530718;
        	const float golden   = 2.39996322973; // the golden angle: a Fibonacci spiral of the stratified radii
        	// The centre itself, weighted by the profile's mass inside half a pixel's footprint stand-in (its first
        	// stratum) - it is always valid, so the sum never empties.
        	vec3 sum     = centre.rgb * BurleyProfile(dMax * 0.05, d) / BurleyProfile1(dMax * 0.05, dMax);
        	vec3 weights = BurleyProfile(dMax * 0.05, d) / BurleyProfile1(dMax * 0.05, dMax);
        	for (int i = 0; i < samples; ++i)
        	{
        		const float u     = (float(i) + 0.5) / float(samples) * 0.995; // 99.5 % of the widest channel's energy
        		const float r     = BurleyInverseCDF(u) * dMax;
        		const float theta = float(i) * golden + rotation;
        		const vec3  world = P + r * (cos(theta) * T + sin(theta) * B);
        		const vec4  clip  = WorldToClip * vec4(world, 1.0);
        		if (clip.w <= 0.0)
        			continue;
        		const vec2 sampleUV = NdcToUV(clip.xy / clip.w);
        		if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThanEqual(sampleUV, vec2(1.0))))
        			continue;
        		const ivec2 at = ivec2(sampleUV * vec2(size));
        		const vec4  s  = texelFetch(SubsurfaceDiffuseTexture, at, 0);
        		if (abs(s.a - centre.a) > 0.25) // another profile, an unscattered surface or the sky
        			continue;
        		const float sampleDepth = texelFetch(SceneDepthTexture, at, 0).r;
        		const vec3  hit = ReconstructWorldPosition((vec2(at) + 0.5) * texel, sampleDepth, ClipToWorld);
        		// UE BurleyNormalizedSS's bilateral radius: the drawn radius r lengthened by how far the surface actually
        		// hit lies off the tangent-plane point that was aimed at (UE: sqrt(r^2 + DeltaDepth^2)), never shortened.
        		// Weighting by the raw |hit - P| instead let a far-drawn sample that lands on a fold close to P (the
        		// ear's concha) take profile(small)/pdf(large r) - unbounded as the radius grows, so at Scale x4 one
        		// shadowed sample owned the texel: the dark dots. With the radius >= r each weight is <= R(r)/pdf(r).
        		const float offPlane = distance(hit, world);
        		const float radius   = sqrt(r * r + offPlane * offPlane);
        		const vec3  w        = BurleyProfile(radius, d) / BurleyProfile1(r, dMax);
        		sum     += w * s.rgb;
        		weights += w;
        	}
        	const vec3  current = sum / max(weights, vec3(1e-6));
        	const float n       = min(frames + 1.0, HistoryParams.z);
        	const float lum     = SubsurfaceLuminance(current);
        	mean   = frames > 0.0 ? mix(mean, current, 1.0 / n) : current;
        	moment = frames > 0.0 ? mix(moment, lum * lum, 1.0 / n) : lum * lum;
        	imageStore(SubsurfaceScatteredTexture, pixel, vec4(mean, 1.0));
        	imageStore(SubsurfaceHistoryOutTexture, pixel, vec4(mean, n));
        	imageStore(SubsurfaceHistoryMomentsOutTexture, pixel, vec4(moment, centre.a, depth, 0.0));
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"dd75641c0956448db3367ad572bec97e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDReconstructShadowVisibility"
{
    // The screen-space denoiser's RECONSTRUCTION of the shadow-visibility signal (UE ScreenSpaceDenoiser, shadow
    // visibility mask: SSDSpatialAccumulation.usf STAGE_RECONSTRUCTION with SIGNAL_PROCESSING_SHADOW_VISIBILITY_MASK,
    // SSDSignalShadowVisibilityMask). The producer hands a penumbra mask (one stochastic visibility estimate per
    // pixel) and the distance to the closest occluder its rays hit; whatever produced them does not matter here.
    // The penumbra of a light of angular diameter theta behind an occluder at distance d is d * tan(theta / 2)
    // wide in the world (UE GetSignalWorldBluringRadius for shadows): that is the kernel's world radius, turned into
    // pixels through the pixel's world footprint. A 5x5 tent kernel spread over that radius, every tap weighed by the
    // BILATERAL weight of its surface (UE BILATERAL_POSITION_BASED(2) + BILATERAL_NORMAL) - a penumbra does not
    // cross a depth edge or a crease. The signal travels as (v, v, v, 1): visibility in rgb, every surface pixel
    // answered (a = 1), so the shared temporal and convolution passes treat it as a diffuse-style signal.

    Compute
    {
        // Shared sampler state (UE shared samplers): each input below is a separate texture read through it, bound
        // once by the pass by name and numbered by the program build (ShaderMapBuild.cpp AssignSharedSamplerBindings).
        // Declared before Common/ScreenSpaceDenoise.glslh, whose SSDLoadSceneInfo reads the G-buffer through it.
        Uniform sampler PointClampSampler;
        #include <Common/ScreenSpaceDenoise.glslh>

        // The G-buffer stays a bare texture2D: SSDLoadSceneInfo takes the textures and constructs at the lookup.
        Uniform(1) texture2D GBufferNormalTexture;   // rgb = world normal, a = roughness
        Uniform(2) texture2D GBufferDepthTexture;    // device depth (nearest)
        Uniform(3) texture2D RawSignalTexture;            // r = visibility estimate, 0 = shadowed, 1 = lit
        #define RawSignalTexture sampler2D(RawSignalTexture, PointClampSampler)
        Uniform(4) texture2D ClosestOccluderTexture; // r = distance to the closest occluder hit, cm; <= 0 = no hit
        #define ClosestOccluderTexture sampler2D(ClosestOccluderTexture, PointClampSampler)
        layout(binding = 6, rgba16f) writeonly uniform image2D ReconstructedSignalTexture; // premultiplied (v, v, v, 1)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 ViewProjection;
        	mat4 PreviousViewProjection;
        	mat4 InverseJitteredViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 Params; // x = tan(light source angle / 2), y = kernel radius in taps per side (Scalability
        	               // Shadows.RayTracedDenoiserRadius: 2 = 5x5), zw unused
        	vec4 u_LocalLight; // a local light's emitter sphere: xyz = position cm, w = source radius cm (0 = the
        	                   // angle in u_Params.x sizes the penumbra)
        };

        LocalSize(8, 8, 1);

        // The widest kernel, in pixels (UE's reconstruction kernel spans at most this many pixels per side for
        // shadows: beyond it the temporal accumulation and the history convolution do the rest).
        const float kMaxKernelPixels = 16.0;

        void main()
        {
        	const ivec2 viewSize = textureSize(sampler2D(GBufferNormalTexture, PointClampSampler), 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	if (!SSDPixelHasSignal(kSSDSignalShadowVisibility, texelFetch(sampler2D(GBufferNormalTexture, PointClampSampler), pixel, 0)))
        	{
        		imageStore(ReconstructedSignalTexture, pixel, vec4(0.0));
        		return;
        	}
        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture, InverseJitteredViewProjection,
        	                                           pixel, size);
        	const float pixelRadius = max(SSDPixelWorldRadius(uv, texelFetch(sampler2D(GBufferDepthTexture, PointClampSampler), pixel, 0).r,
        	                                                  ref.Position, 1.0 / size, InverseJitteredViewProjection),
        	                              1e-4);

        	// The closest occluder around the pixel: a lit pixel next to a penumbra has no hit of its own but lies
        	// inside the penumbra its neighbours' occluder casts.
        	float occluder = 0.0;
        	for (int y = -1; y <= 1; ++y)
        		for (int x = -1; x <= 1; ++x)
        		{
        			const float d = texelFetch(ClosestOccluderTexture, clamp(pixel + ivec2(x, y), ivec2(0), viewSize - 1), 0).r;
        			if (d > 0.0)
        				occluder = occluder > 0.0 ? min(occluder, d) : d;
        		}
        	// A sphere light of radius R at distance d behind an occluder at t from the receiver: a penumbra
        	// R * t / (d - t) wide; a directional light: t * tan(angle / 2).
        	const float penumbra = u_LocalLight.w > 0.0
        	                           ? occluder * u_LocalLight.w / max(distance(ref.Position, u_LocalLight.xyz) - occluder, 1e-3)
        	                           : occluder * u_Params.x;
        	const float kernelPixels = clamp(penumbra / pixelRadius, 0.0, kMaxKernelPixels);
        	if (kernelPixels < 0.5)
        	{
        		const float v = texelFetch(RawSignalTexture, pixel, 0).r;
        		imageStore(ReconstructedSignalTexture, pixel, vec4(v, v, v, 1.0));
        		return;
        	}

        	const float maxPlaneDistance = max(pixelRadius * kernelPixels * 2.0, 1e-3);
        	// The (2R+1)^2 taps span +-R strides = the penumbra's radius; a tent weight of R+1-|offset|.
        	const int   radius           = int(Params.y + 0.5);
        	const float tent             = float(radius + 1);
        	const float stride           = kernelPixels / float(radius);
        	float       sum              = 0.0;
        	float       weightSum        = 0.0;
        	for (int y = -radius; y <= radius; ++y)
        		for (int x = -radius; x <= radius; ++x)
        		{
        			const ivec2 p = clamp(pixel + ivec2(round(vec2(x, y) * stride)), ivec2(0), viewSize - 1);
        			const SSDSceneInfo other = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture,
        			                                            InverseJitteredViewProjection, p, size);
        			if (dot(other.Normal, other.Normal) <= 0.0)
        				continue; // sky
        			const float w = (tent - abs(float(x))) * (tent - abs(float(y))) *
        			                SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        			                SSDNormalWeight(ref.Normal, other.Normal);
        			sum       += texelFetch(RawSignalTexture, p, 0).r * w;
        			weightSum += w;
        		}
        	const float v = weightSum > 0.0 ? sum / weightSum : texelFetch(RawSignalTexture, pixel, 0).r;
        	imageStore(ReconstructedSignalTexture, pixel, vec4(v, v, v, 1.0));
        }
    }
}

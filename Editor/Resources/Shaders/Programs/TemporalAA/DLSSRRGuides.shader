// DesertAsset {"Kind":"Shader","Guid":"d302b98ff5224b06af5d0cd0eb20391f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DLSSRRGuides"
{
    Compute
    {
        // THE GUIDES DLSS RAY RECONSTRUCTION READS BESIDE THE COLOUR (Graphic::DLSSUpscaler, "DLSS-RR: Guides";
        // UE's DLSS plugin builds the same set from its GBuffer in its "DLSS-RR GBuffer resolve" pass). Render
        // extent, one thread per pixel:
        //   DiffuseAlbedoTexture  - what the composite multiplied the diffuse light by: (1 - F) * (1 - metallic) *
        //                      base colour (Mesh/AmbientIBL.glslh EvaluateAmbientIBL's kd * albedo);
        //   SpecularAlbedoTexture - what it multiplied the reflection by: the split-sum reflectance F0 * A + B, A and B
        //                      from the BRDF table at (N.V, roughness) (EvaluateAmbientIBL's specularIBL weight);
        //   SpecularHitDistanceTexture - how far the pixel's traced reflection travelled (world units, centimetres):
        //                      from the pixel to the point SSR's ray reflected (ReflectionRayTexture.xyz). 0 where the
        //                      pixel traced no ray. Written only when the view traced (Params.z).
        // The demodulation is exact only when the guides are the factors the colour was made with, which is why
        // they are computed here from the same G-buffer, F0 rule and table as the lighting, never approximated.
        // The normal + roughness guide is the G-buffer's own normal target (rgb world normal, a roughness), tagged
        // as it is (sl::DLSSDNormalRoughnessMode::ePacked). Background pixels (far depth) get zero guides.

        #include <Common/ReconstructPosition.glslh>
        #include <Mesh/BRDF.glslh>

        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(8) sampler PointClampSampler;
        Uniform(9) sampler LinearClampSampler;
        Uniform(0) texture2D GBufferAlbedoTexture;  // rgb base colour, a metallic (ViewTargetFormats::kGBufferA)
        #define GBufferAlbedoTexture sampler2D(GBufferAlbedoTexture, PointClampSampler)
        Uniform(1) texture2D GBufferNormalTexture;  // rgb world normal, a roughness (kGBufferB)
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        Uniform(2) texture2D GBufferDepthTexture;   // device depth, reversed-Z (0 = far)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(3) texture2D BrdfLutTexture; // split-sum table: u = N.V, v = roughness; rg = A, B
        #define BrdfLutTexture sampler2D(BrdfLutTexture, LinearClampSampler)
        Uniform(4) texture2D ReflectionRayTexture;  // SSR full-resolution trace ray, or System.Black (Params.z = 0)
        #define ReflectionRayTexture sampler2D(ReflectionRayTexture, PointClampSampler)
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D DiffuseAlbedoTexture;
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D SpecularAlbedoTexture;
        layout(binding = 7, r32f) restrict writeonly uniform image2D SpecularHitDistanceTexture;

        PushConstant PushConstants
        {
        	mat4 InverseJitteredViewProjection; // the matrix the G-buffer depth was rasterised with, inverted
        	vec4 CameraPosition;                 // xyz camera world position
        	vec4 Params;                    // xy render extent, z = 1 when ReflectionRayTexture is this frame's trace
        };

        // DeferredLighting.shader's Fdielectric: the base reflectance of a dielectric (Lit.glsl.frag's too).
        const vec3 kDielectricF0 = vec3(0.04);

        void main()
        {
        	const ivec2 pixel  = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 extent = ivec2(Params.xy);
        	if (pixel.x >= extent.x || pixel.y >= extent.y)
        		return;

        	const float depth = texelFetch(GBufferDepthTexture, pixel, 0).r;
        	if (depth <= 0.0)
        	{
        		imageStore(DiffuseAlbedoTexture, pixel, vec4(0.0));
        		imageStore(SpecularAlbedoTexture, pixel, vec4(0.0));
        		imageStore(SpecularHitDistanceTexture, pixel, vec4(0.0));
        		return;
        	}
        	const vec4  albedo    = texelFetch(GBufferAlbedoTexture, pixel, 0);
        	const vec4  normal    = texelFetch(GBufferNormalTexture, pixel, 0);
        	const vec2  uv        = (vec2(pixel) + 0.5) / vec2(extent);
        	const vec3  world     = ReconstructWorldPosition(uv, depth, InverseJitteredViewProjection);
        	const vec3  V         = normalize(CameraPosition.xyz - world);
        	const vec3  N         = normalize(normal.xyz);
        	const float roughness = normal.a;
        	const float metallic  = albedo.a;
        	const float cosLo     = max(0.0, dot(N, V));

        	const vec3 F0 = mix(kDielectricF0, albedo.rgb, metallic);
        	const vec3 F  = fresnelSchlick(F0, cosLo);
        	const vec2 AB = textureLod(BrdfLutTexture, vec2(cosLo, roughness), 0.0).rg;
        	imageStore(DiffuseAlbedoTexture, pixel, vec4((vec3(1.0) - F) * (1.0 - metallic) * albedo.rgb, 1.0));
        	imageStore(SpecularAlbedoTexture, pixel, vec4(F0 * AB.x + AB.y, 1.0));

        	float hitDistance = 0.0;
        	if (Params.z > 0.5)
        	{
        		const vec4 ray = texelFetch(ReflectionRayTexture, pixel, 0);
        		// w = log2 pdf of the ray; -126 marks a pixel that traced none (SSR.shader TraceFrom).
        		if (ray.w > -126.0)
        			hitDistance = distance(world, ray.xyz);
        	}
        	imageStore(SpecularHitDistanceTexture, pixel, vec4(hitDistance, 0.0, 0.0, 0.0));
        }
    }
}

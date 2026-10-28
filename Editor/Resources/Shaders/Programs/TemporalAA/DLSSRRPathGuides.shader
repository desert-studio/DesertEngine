// DesertAsset {"Kind":"Shader","Guid":"cda7e91534c645b28dc12d40b7e34069","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DLSSRRPathGuides"
{
    Compute
    {
        // THE GUIDES DLSS RAY RECONSTRUCTION READS ON A PATH-TRACED VIEW (Graphic::DLSSUpscaler, "DLSS-RR: Path
        // Guides"; the raster view's are DLSSRRGuides.shader). The colour was made by this frame's paths, each
        // D_d * I_d + D_s * I_s at its first surface (Common/PathTracingDenoise.glslh), so the guides that
        // demodulate it exactly are the paths' own factors, not the G-buffer's: the path tracer's guide sums of
        // this frame (System::PathTracer, PathTracedGuides) divided by s, the pixel's surface paths. Render extent,
        // one thread per pixel:
        //   DiffuseAlbedoTexture       - D_d = base colour x (1 - metallic), mean over the surface paths;
        //   SpecularAlbedoTexture      - D_s = the split-sum reflectance F0 x A + B, mean over the surface paths;
        //   NormalRoughnessTexture     - rgb the mean first-surface world normal (normalised), a the mean
        //                                roughness: the G-buffer's layout (kGBufferB, signed), tagged the same way
        //                                (sl::DLSSDNormalRoughnessMode::ePacked);
        //   SpecularHitDistanceTexture - the first bounce's length weighted by its specular share (world units,
        //                                centimetres; kPathTracingDenoiseMissHitDistance for a bounce that left the
        //                                scene), 0 where no specular share was traced.
        // A pixel no path of which met a surface (s = 0: the sky) gets zero guides, as the raster's background.

        LocalSize(8, 8, 1);

        // The view's guide sums (ViewTargetFormats::kPathTracingAccumulation), read as the path tracer's own
        // passes read them: storage images, the accesses its accumulation allows.
        layout(binding = 0, rgba32f) restrict readonly uniform image2D GuideAlbedoTexture;   // rgb sum D_d, a s
        layout(binding = 1, rgba32f) restrict readonly uniform image2D GuideNormalTexture;   // xyz sum N, w sum depth
        layout(binding = 2, rgba32f) restrict readonly uniform image2D GuideSpecularTexture; // rgb sum D_s, a sum rough
        layout(binding = 3, rgba32f) restrict readonly uniform image2D GuideHitTexture;      // x sum w t, y sum w
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D DiffuseAlbedoTexture;
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D SpecularAlbedoTexture;
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D NormalRoughnessTexture;
        layout(binding = 7, r32f) restrict writeonly uniform image2D SpecularHitDistanceTexture;

        PushConstant PushConstants
        {
        	vec4 Params; // xy render extent, zw unused
        };

        void main()
        {
        	const ivec2 pixel  = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 extent = ivec2(Params.xy);
        	if (pixel.x >= extent.x || pixel.y >= extent.y)
        		return;

        	const vec4  albedo = imageLoad(GuideAlbedoTexture, pixel);
        	const float paths  = albedo.a;
        	if (paths <= 0.0)
        	{
        		imageStore(DiffuseAlbedoTexture, pixel, vec4(0.0));
        		imageStore(SpecularAlbedoTexture, pixel, vec4(0.0));
        		imageStore(NormalRoughnessTexture, pixel, vec4(0.0));
        		imageStore(SpecularHitDistanceTexture, pixel, vec4(0.0));
        		return;
        	}
        	const float inv      = 1.0 / paths;
        	const vec4  normal   = imageLoad(GuideNormalTexture, pixel);
        	const vec4  specular = imageLoad(GuideSpecularTexture, pixel);
        	const vec4  hit      = imageLoad(GuideHitTexture, pixel);

        	// The paths' sub-pixel normals average to a shorter vector on a curved or bumpy pixel; its direction
        	// is the pixel's normal. A zero sum (normals cancelling exactly) has no direction to give.
        	const float normalLength = length(normal.xyz);
        	const vec3  N            = normalLength > 0.0 ? normal.xyz / normalLength : vec3(0.0);

        	imageStore(DiffuseAlbedoTexture, pixel, vec4(albedo.rgb * inv, 1.0));
        	imageStore(SpecularAlbedoTexture, pixel, vec4(specular.rgb * inv, 1.0));
        	imageStore(NormalRoughnessTexture, pixel, vec4(N, specular.a * inv));
        	imageStore(SpecularHitDistanceTexture, pixel, vec4(hit.y > 0.0 ? hit.x / hit.y : 0.0, 0.0, 0.0, 0.0));
        }
    }
}

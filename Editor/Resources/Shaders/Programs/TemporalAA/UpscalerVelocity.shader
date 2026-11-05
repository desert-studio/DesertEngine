// DesertAsset {"Kind":"Shader","Guid":"f83831c4ff5e4a9fba4393c0564dc8da","Versions":{"SHDR":1},"Dependencies":[]}
Shader "UpscalerVelocity"
{
    Compute
    {
        // The motion vectors a vendor temporal upscaler takes (Graphic::UpscalerVelocity; UE's DLSS plugin
        // VelocityCombine, its FSR plugin ConvertVelocity). The view's velocity (View/SceneViewState.hpp) is written
        // only where geometry rasterised; the sky, the clouds and the far plane keep the clear value 0, and the
        // engine's own resolves derive their motion from depth (TemporalAA.shader's sky branch, TSRDilateVelocity).
        // DLSS and FSR read the image as given (cameraMotionIncluded): a 0 there means "this pixel sat at the same
        // screen point last frame", so under a camera turn the sky's history stays glued to the screen and the
        // horizon trails the camera. This pass writes the camera's motion at infinity into every pixel nothing wrote
        // velocity for (velocity 0, reversed-Z depth 0), one formula with TAA (Common/Velocity.glslh
        // DesertCameraVelocityAtInfinity), and copies every other pixel unchanged (xyzw, the object-motion flag too).

        LocalSize(8, 8, 1);

        #include <Common/ScreenUV.glslh>
        #include <Common/Velocity.glslh>

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler PointClampSampler;
        Uniform(0) texture2D VelocityTexture;   // NDC current - previous, RenderExtent (ViewTargetFormats::kVelocity)
        #define VelocityTexture sampler2D(VelocityTexture, PointClampSampler)
        Uniform(1) texture2D SceneDepthTexture; // reversed-Z device depth, RenderExtent (0 = far)
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D UpscalerVelocityTexture;

        PushConstant PushConstants
        {
        	mat4 ClipToPrevClip; // PreviousViewProjection * InverseViewProjection, both unjittered
        	vec4 Params;         // xy render extent
        };

        void main()
        {
        	const ivec2 pixel  = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 extent = ivec2(Params.xy);
        	if (pixel.x >= extent.x || pixel.y >= extent.y)
        		return;

        	vec4        velocity = texelFetch(VelocityTexture, pixel, 0);
        	const float depth    = texelFetch(SceneDepthTexture, pixel, 0).r;
        	if (velocity.xy == vec2(0.0) && depth <= 0.0)
        	{
        		const vec2 uv = (vec2(pixel) + 0.5) / Params.xy;
        		velocity      = vec4(DesertCameraVelocityAtInfinity(ScreenUVToNdc(uv), ClipToPrevClip), 0.0, 0.0);
        	}
        	imageStore(UpscalerVelocityTexture, pixel, velocity);
        }
    }
}

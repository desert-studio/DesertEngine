// DesertAsset {"Kind":"Shader","Guid":"eb0326a27ff84674a06137be54aea2d5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "RayTracedLocalShadowPack"
{
    // "Shadows: Ray Traced Local Pack": one slot's (denoised) visibility into its layer of the frame's 2D-array
    // local shadow mask (RayTracedLocalShadow.hpp), which the composite's DESERT_LOCAL_SHADOW_SCREEN_MASK program
    // reads by the light's ShadowMaskSlot. The source is read at the pixel's uv, so a 1x1 System.White (no slot, or a
    // slot whose trace could not be declared) fills the layer with "lit".

    Compute
    {
        Uniform sampler PointClampSampler;
        Uniform(1) texture2D VisibilityTexture; // r = visibility, 1 = lit
        layout(binding = 2, rgba16f) writeonly uniform image2DArray LocalShadowMasksTexture;

        // RayTracedLocalShadowPackPush (RayTracedLocalShadow.hpp).
        PushConstant PushConstants
        {
        	ivec4 GatheredLayerTexture; // x = the slot's layer
        };

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 size  = imageSize( LocalShadowMasksTexture ).xy;
            if ( pixel.x >= size.x || pixel.y >= size.y )
                return;
            const vec2  uv         = ( vec2( pixel ) + 0.5 ) / vec2( size );
            const float visibility = textureLod( sampler2D( VisibilityTexture, PointClampSampler ), uv, 0.0 ).r;
            // The bound view is this slot's layer alone (StorageLayer): its layer 0.
            imageStore( LocalShadowMasksTexture, ivec3( pixel, 0 ), vec4( visibility ) );
        }
    }
}

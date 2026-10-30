// DesertAsset {"Kind":"Shader","Guid":"a83d6f2e0c5b4f91b7e2d94c6f13a0b8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapProjectLocal"
{
    // "VSM: Project Local" (UE VirtualShadowMapProjection.usf for a local light): one dispatch layer per shadowed
    // point / spot light of this frame writes its screen shadow mask into layer Views.z of the frame's local mask
    // array, which the deferred composite reads through the light's ShadowMaskSlot (Mesh/LocalLightShadow.glslh,
    // DESERT_LOCAL_SHADOW_SCREEN_MASK) - the same array "Shadows: Ray Traced Local" fills under HardwareTraced.
    // The reader is the one every receiver of a local light's map shares (Common/VirtualShadowMap.glslh,
    // VsmLocalVisibility).
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/VirtualShadowMap.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        ReadBuffer(1) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        layout(binding = 2, r32ui) restrict readonly uniform uimage2D PhysicalPoolTexture;
        Uniform sampler PointClampSampler;
        Uniform(3) texture2D GBufferDepthTexture;  // device depth, reversed-Z (0 = far: no surface)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(4) texture2D GBufferNormalTexture; // rgb = world normal
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        ReadBuffer(5) VsmLocalLights
        {
            VsmLocalLight Lights[];
        };
        layout(binding = 6, rgba16f) writeonly uniform image2DArray LocalShadowMasksTexture; // r = visibility, 1 = lit
        #define DESERT_VSM_READ
        #include <Common/VirtualShadowMap.glslh>

        PushConstant PushConstants
        {
            uvec4 RenderSize; // xy = render size, z = local light count
        };

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            const uint  index = gl_GlobalInvocationID.z;
            if ( pixel.x >= int( RenderSize.x ) || pixel.y >= int( RenderSize.y ) || index >= RenderSize.z )
                return;
            const VsmLocalLight light      = Lights[index];
            float               visibility = 1.0;
            const float         depth      = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            if ( depth > 0.0 )
            {
                const vec2 uv    = ( vec2( pixel ) + 0.5 ) / vec2( RenderSize.xy );
                const vec3 world = ReconstructWorldPosition( uv, depth, Frame.InvViewProjection );
                if ( VsmLocalReaches( light, world ) )
                    visibility =
                         VsmLocalVisibility( light, world, normalize( texelFetch( GBufferNormalTexture, pixel, 0 ).rgb ) );
            }
            imageStore( LocalShadowMasksTexture, ivec3( pixel, int( light.Views.z ) ), vec4( visibility ) );
        }
    }
}

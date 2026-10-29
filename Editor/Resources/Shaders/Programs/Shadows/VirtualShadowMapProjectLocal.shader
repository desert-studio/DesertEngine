// DesertAsset {"Kind":"Shader","Guid":"a83d6f2e0c5b4f91b7e2d94c6f13a0b8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapProjectLocal"
{
    // "VSM: Project Local" (UE VirtualShadowMapProjection.usf for a local light): one dispatch layer per shadowed
    // point / spot light of this frame writes its screen shadow mask into layer Views.z of the frame's local mask
    // array, which the deferred composite reads through the light's ShadowMaskSlot (Mesh/LocalLightShadow.glslh,
    // DESERT_LOCAL_SHADOW_SCREEN_MASK) - the same array "Shadows: Ray Traced Local" fills under HardwareTraced.
    // The receiver is offset along its normal by a texel of its mip (more as the light grazes it), then a 3 x 3
    // bilinear PCF over the texels of that mip; a tap whose page is not mapped reads the next coarser mip (the
    // coarsest is always mapped, "VSM: Mark Local"). Depths are compared as distances along the view axis, with a
    // bias of one texel of the receiver's mip.
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

        PushConstant PushConstants
        {
            uvec4 RenderSize; // xy = render size, z = local light count
        };

        // The view distance of the nearest occluder over texel @p texel of mip @p mip of the view whose mip 0 is
        // level @p first, falling back to coarser mips where the page holds no current depth; the far plane where no
        // mip holds one (nothing drawn there: no occluder).
        float OccluderDistance( VsmLocalLight light, uint first, uint mip, ivec2 texel )
        {
            for ( uint m = mip; m < VSM_LOCAL_MIPS; ++m )
            {
                const uint  level = first + m;
                const int   size  = int( Levels[level].Pages.y * VSM_PAGE_SIZE );
                const ivec2 t     = clamp( texel >> int( m - mip ), ivec2( 0 ), ivec2( size - 1 ) );
                const VsmPageEntry entry =
                     Entries[VsmLevelEntryIndex( Levels[level], level, t / int( VSM_PAGE_SIZE ) )];
                // Mapped and current: drawn this frame (dirty and requested) or cached (not dirty).
                if ( ( entry.Flags & VSM_FLAG_ALLOCATED ) == 0u ||
                     ( entry.Flags & ( VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED ) ) == VSM_FLAG_DIRTY )
                    continue;
                const uint bits =
                     imageLoad( PhysicalPoolTexture,
                                VsmPoolPageOrigin( entry.Physical ) + ( t & ivec2( int( VSM_PAGE_SIZE ) - 1 ) ) ).r;
                const float z = 1.0 - uintBitsToFloat( bits );
                return z >= 1.0 ? light.Params.y : VsmLocalLinearDepth( z, light.Params.x, light.Params.y );
            }
            return light.Params.y;
        }

        float Visibility( VsmLocalLight light, vec3 world, vec3 normal )
        {
            const uint first = light.Views.x + VsmLocalView( light, world ) * VSM_LOCAL_MIPS;
            const mat4 vp    = Levels[first].ViewProjection;
            const vec4 clip0 = vp * vec4( world, 1.0 );
            if ( clip0.w <= light.Params.x )
                return 1.0;
            const uint  mip     = VsmLocalMip( Frame, light, world, clip0.w );
            const float texelAt = light.Params.z * float( 1u << mip ) * clip0.w; // the mip's texel here, cm
            const vec3  L       = normalize( light.Position.xyz - world );
            const float NdotL   = clamp( dot( normal, L ), 0.0, 1.0 );
            const vec3  offset  = world + normal * texelAt * ( 0.5 + 1.5 * ( 1.0 - NdotL ) );
            const vec4  clip    = vp * vec4( offset, 1.0 );
            if ( clip.w <= light.Params.x )
                return 1.0;
            const vec2 ndc = clip.xy / clip.w;
            if ( any( greaterThan( abs( ndc ), vec2( 1.0 ) ) ) )
                return 1.0;
            const float size     = float( Levels[first + mip].Pages.y * VSM_PAGE_SIZE );
            const vec2  texel    = ( ndc * 0.5 + 0.5 ) * size - 0.5;
            const vec2  base     = floor( texel );
            const vec2  f        = texel - base;
            const float receiver = clip.w - texelAt;
            // 3 x 3 bilinear PCF: 4 x 4 taps with tent weights (the atlas's LocalShadowPCF shape).
            const vec4 wx  = vec4( 1.0 - f.x, 1.0, 1.0, f.x );
            const vec4 wy  = vec4( 1.0 - f.y, 1.0, 1.0, f.y );
            float      lit = 0.0;
            for ( int j = 0; j < 4; ++j )
            {
                for ( int i = 0; i < 4; ++i )
                {
                    const ivec2 t = ivec2( base ) + ivec2( i - 1, j - 1 );
                    lit += wx[i] * wy[j] * ( OccluderDistance( light, first, mip, t ) >= receiver ? 1.0 : 0.0 );
                }
            }
            return lit / 9.0;
        }

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
                         Visibility( light, world, normalize( texelFetch( GBufferNormalTexture, pixel, 0 ).rgb ) );
            }
            imageStore( LocalShadowMasksTexture, ivec3( pixel, int( light.Views.z ) ), vec4( visibility ) );
        }
    }
}

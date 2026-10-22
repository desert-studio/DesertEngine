// DesertAsset {"Kind":"Shader","Guid":"37dbac6cfd7b41c6b6521a8d50477ad9","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapProject"
{
    // "VSM: Project" (UE VirtualShadowMapProjection.usf + VirtualShadowMapSMRTCommon.ush): the sun's screen shadow
    // mask (UE FScreenShadowMaskTexture, the same ShadowMask.Sun the ray-traced sun writes), read by the composite
    // and the forward programs in place of the cascades.
    //
    // SHADOW MAP RAY TRACING: per pixel Frame.Smrt.x rays inside the sun's SourceAngle cone (Common/ConeSampling),
    // each marched toward the sun over Frame.Smrt.y samples (denser near the receiver) up to Smrt.w pages of the
    // receiver's level. A sample is blocked when the virtual shadow map holds an occluder in front of it (light
    // depth); a sample on an unmapped page falls back to the next coarser level's page, and lights when none of
    // them is mapped (UE: an unmapped page casts nothing). The mask is the fraction of rays that reach the sun:
    // the penumbra widens with the occluder's distance as the cone does. No rays (Smrt.x = 0) or a zero angle: one
    // hard lookup at the receiver.
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/VirtualShadowMap.glslh>
        #include <Common/ConeSampling.glslh>
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
        Uniform(4) texture2D GBufferNormalTexture; // rgb = world normal, a = roughness
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        layout(binding = 5, rgba16f) writeonly uniform image2D ShadowMaskTexture; // r = visibility, 1 = lit

        // The receiver's own depth may sit this many texels behind the stored occluder depth and still be lit (the
        // texel's extent along a surface facing the sun; the caster's slope bias covers the rest).
        #define VSM_DEPTH_BIAS_TEXELS 1.0

        // 1 = the point at @p world sees the sun along the light direction, 0 = an occluder of the map is in front.
        float VisibleAt( uint firstLevel, vec3 world )
        {
            const uint levels = uint( Frame.Lod.y );
            for ( uint level = firstLevel; level < levels; ++level )
            {
                vec2  texel;
                float depth;
                if ( !VsmProject( Levels[level], world, texel, depth ) )
                    continue;
                const ivec2        windowTexel = ivec2( texel );
                const VsmPageEntry entry = Entries[VsmEntryIndex(
                     level, Levels[level].Window.xy + windowTexel / int( VSM_PAGE_SIZE ) )];
                // Mapped, and either clean or drawn this frame (requested and dirty = cleared and drawn now).
                const bool mapped = ( entry.Flags & VSM_FLAG_ALLOCATED ) != 0u &&
                                    ( ( entry.Flags & VSM_FLAG_DIRTY ) == 0u || ( entry.Flags & VSM_FLAG_REQUESTED ) != 0u );
                if ( !mapped )
                    continue;
                const uint bits = imageLoad( PhysicalPoolTexture, VsmPoolPageOrigin( entry.Physical ) +
                                                                  ( windowTexel & ivec2( int( VSM_PAGE_SIZE ) - 1 ) ) ).r;
                const float occluder = 1.0 - uintBitsToFloat( bits ); // 0 bits = 1.0 = no occluder
                const float bias     = VSM_DEPTH_BIAS_TEXELS * Levels[level].Extent.w / Levels[level].Extent.y;
                return occluder < depth - bias ? 0.0 : 1.0;
            }
            return 1.0;
        }

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 size  = imageSize( ShadowMaskTexture );
            if ( pixel.x >= size.x || pixel.y >= size.y )
                return;
            const float depth = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            if ( depth <= 0.0 )
            {
                imageStore( ShadowMaskTexture, pixel, vec4( 1.0 ) );
                return;
            }
            const vec2 uv       = ( vec2( pixel ) + 0.5 ) / vec2( size );
            // Common/ReconstructPosition.glslh: (0,0) is the top-left pixel and NDC +y is up.
            const vec3 position = ReconstructWorldPosition( uv, depth, Frame.InvViewProjection );
            const vec3 normal   = normalize( texelFetch( GBufferNormalTexture, pixel, 0 ).rgb );
            const vec3 toSun    = normalize( Frame.ToSun.xyz );

            const uint  level      = VsmSelectLevel( Frame, position );
            const float texelWorld = Levels[level].Extent.w;
            // UE r.Shadow.Virtual.NormalBias: off the surface along its normal, on the side facing the sun, in texels
            // of the receiver's level.
            const vec3 origin =
                 position + normal * ( dot( normal, toSun ) >= 0.0 ? 1.0 : -1.0 ) * ( texelWorld * Frame.Smrt.z );

            const uint  rays      = uint( Frame.Smrt.x );
            const float halfAngle = 0.5 * Frame.ToSun.w;
            if ( rays == 0u || halfAngle <= 0.0 )
            {
                imageStore( ShadowMaskTexture, pixel, vec4( VisibleAt( level, origin ) ) );
                return;
            }

            const uint  samples   = max( uint( Frame.Smrt.y ), 1u );
            const float rayLength = Frame.Smrt.w * Levels[level].Extent.x;
            const uint  frame     = uint( Frame.Lod.w );
            float       visible   = 0.0;
            for ( uint r = 0u; r < rays; ++r )
            {
                const vec2  u      = ConeRandom2( uvec2( pixel ), frame, r );
                const vec3  dir    = ConeDirection( toSun, halfAngle, u );
                const float jitter = ConeRandom2( uvec2( pixel ), frame, r + rays ).x;
                float       lit    = 1.0;
                for ( uint s = 0u; s < samples && lit > 0.0; ++s )
                {
                    const float t = ( float( s ) + jitter ) / float( samples );
                    lit           = VisibleAt( level, origin + dir * ( rayLength * t * t ) );
                }
                visible += lit;
            }
            imageStore( ShadowMaskTexture, pixel, vec4( visible / float( rays ) ) );
        }
    }
}

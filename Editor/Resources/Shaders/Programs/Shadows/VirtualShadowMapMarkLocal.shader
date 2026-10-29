// DesertAsset {"Kind":"Shader","Guid":"5c1e0a3f9b7d4e2a8f60c4d2b1a97e35","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapMarkLocal"
{
    // "VSM: Mark Local" (UE VirtualShadowMapPageMarking.usf for local lights): every G-buffer pixel a shadowed point /
    // spot light of this frame reaches requests, in the view of that light the pixel is seen through (the spot's one
    // view, the point light's cube face), the page of the mip its footprint asks for (VsmLocalMip), and the light's
    // coarsest page (UE MarkCoarsePagesLocal): the projection falls back to a coarser mip where a filter tap leaves
    // the requested page, and the coarsest one is always mapped.
    // The same kernel marks for the other receivers (RenderSize.w, VSM_MARK_*): "VSM: Mark Receivers" runs it over a
    // forward receiver's own depth (the translucent front layer: UE marks translucency from its front layer), flagging
    // its pages VSM_FLAG_RECEIVER; "VSM: Mark Coarse" requests every page of each light's coarse mip
    // (VSM_LOCAL_COARSE_MIP, UE MarkCoarsePagesLocal), one invocation per page, for the readers with no depth to mark
    // from (the surface cache's off-screen cards), flagging them VSM_FLAG_COARSE.
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
        Buffer(1) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        Uniform sampler PointClampSampler;
        Uniform(2) texture2D GBufferDepthTexture; // device depth, reversed-Z (0 = far: no surface)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        ReadBuffer(3) VsmLocalLights
        {
            VsmLocalLight Lights[];
        };

        PushConstant PushConstants
        {
            uvec4 RenderSize; // xy = render size, z = local light count, w = VSM_MARK_*
        };
        #define VSM_MARK_PIXELS    0u // the G-buffer's depth
        #define VSM_MARK_RECEIVERS 1u // a forward receiver's depth
        #define VSM_MARK_COARSE    2u // x = page of the coarse mip, y = view, z = light

        void Request( uint level, ivec2 page, uint kind )
        {
            const uint index = VsmLevelEntryIndex( Levels[level], level, page );
            const uint want  = VSM_FLAG_REQUESTED | kind;
            if ( ( Entries[index].Flags & want ) != want )
                atomicOr( Entries[index].Flags, want );
        }

        // Every page of light @p lightIndex's coarse mip: @p id.x = the page within its view's mip, @p id.y = the view.
        void MarkCoarse( uint lightIndex, uvec2 id )
        {
            if ( lightIndex >= RenderSize.z )
                return;
            const VsmLocalLight light = Lights[lightIndex];
            const uint          mip   = max( VSM_LOCAL_COARSE_MIP, light.Views.w );
            if ( id.y >= light.Views.y )
                return;
            const uint level = light.Views.x + id.y * VSM_LOCAL_MIPS + mip;
            const uint side  = Levels[level].Pages.y;
            if ( id.x >= side * side )
                return;
            Request( level, ivec2( int( id.x % side ), int( id.x / side ) ), VSM_FLAG_COARSE );
        }

        void main()
        {
            if ( RenderSize.w == VSM_MARK_COARSE )
            {
                MarkCoarse( gl_GlobalInvocationID.z, gl_GlobalInvocationID.xy );
                return;
            }
            const uint  kind  = RenderSize.w == VSM_MARK_RECEIVERS ? VSM_FLAG_RECEIVER : 0u;
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            if ( pixel.x >= int( RenderSize.x ) || pixel.y >= int( RenderSize.y ) )
                return;
            const float depth = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            if ( depth <= 0.0 )
                return;
            const vec2 uv    = ( vec2( pixel ) + 0.5 ) / vec2( RenderSize.xy );
            const vec3 world = ReconstructWorldPosition( uv, depth, Frame.InvViewProjection );

            for ( uint i = 0u; i < RenderSize.z; ++i )
            {
                const VsmLocalLight light = Lights[i];
                if ( !VsmLocalReaches( light, world ) )
                    continue;
                const uint first = light.Views.x + VsmLocalView( light, world ) * VSM_LOCAL_MIPS;
                const vec4 clip  = Levels[first].ViewProjection * vec4( world, 1.0 );
                if ( clip.w <= light.Params.x )
                    continue;
                const vec2 ndc = clip.xy / clip.w;
                if ( any( greaterThan( abs( ndc ), vec2( 1.0 ) ) ) )
                    continue;
                const uint  level = first + VsmLocalMip( Frame, light, world, clip.w );
                const float side  = float( Levels[level].Pages.y );
                Request( level, ivec2( min( ( ndc * 0.5 + 0.5 ) * side, vec2( side - 1.0 ) ) ), kind );
                Request( first + VSM_LOCAL_MIPS - 1u, ivec2( 0 ), kind );
            }
        }
    }
}

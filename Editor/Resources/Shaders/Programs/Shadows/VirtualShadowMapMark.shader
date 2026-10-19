// DesertAsset {"Kind":"Shader","Guid":"ab9cac9369dc4c059ec2d18b1bf308ee","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapMark"
{
    // "VSM: Mark" (UE VirtualShadowMapPageMarking.usf, MarkPixelPages): every G-buffer pixel requests the page its
    // receiver is shaded from (VsmSelectLevel) and the page of the next coarser level over it, the one the
    // projection's shadow-map rays fall back to where a ray leaves the requested page.
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/VirtualShadowMap.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame u_Frame;
            VsmLevel u_Levels[];
        };
        Buffer(1) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        Uniform sampler u_PointClampSampler;
        Uniform(2) texture2D u_GBufferDepth; // device depth, reversed-Z (0 = far: no surface)
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)

        PushConstant PushConstants
        {
            uvec4 u_Size; // xy = render size
        };

        void MarkPage( uint level, vec3 world )
        {
            vec2  texel;
            float depth;
            if ( !VsmProject( u_Levels[level], world, texel, depth ) )
                return;
            const ivec2 windowPage = ivec2( texel ) / int( VSM_PAGE_SIZE );
            const uint  index      = VsmEntryIndex( level, u_Levels[level].Window.xy + windowPage );
            // Most pixels of a page find it requested already: read before the atomic (UE does the same).
            if ( ( u_Entries[index].Flags & VSM_FLAG_REQUESTED ) == 0u )
                atomicOr( u_Entries[index].Flags, VSM_FLAG_REQUESTED );
        }

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            if ( pixel.x >= int( u_Size.x ) || pixel.y >= int( u_Size.y ) )
                return;
            const float depth = texelFetch( u_GBufferDepth, pixel, 0 ).r;
            if ( depth <= 0.0 )
                return;
            const vec2 uv       = ( vec2( pixel ) + 0.5 ) / vec2( u_Size.xy );
            // Common/ReconstructPosition.glslh: (0,0) is the top-left pixel and NDC +y is up.
            const vec3 position = ReconstructWorldPosition( uv, depth, u_Frame.InvViewProjection );

            const uint first = VsmSelectLevel( u_Frame, position );
            const uint last  = min( first + 1u, uint( u_Frame.Lod.y ) - 1u );
            for ( uint level = first; level <= last; ++level )
                MarkPage( level, position );
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"8d42abddc8ce465db218b88154406ebc","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapClear"
{
    // "VSM: Clear Pages" (UE ClearPhysicalPages, and the static-cache merge of r.Shadow.Virtual.Cache.StaticSeparate):
    // one group per dirty page of the frame, dispatched indirectly from "VSM: Clear Args". A page whose static layer is
    // redrawn (VSM_FLAG_DIRTY_STATIC) has both layers' 128 x 128 texels go to 0 = no occluder, the value the casters'
    // atomic max then raises; a page whose movers alone changed gets its static layer copied into its merged layer,
    // and only the movers are drawn over it ("VSM: Raster Movable L<l>").
    Compute
    {
        LocalSize(16, 16, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        ReadBuffer(1) VsmLists
        {
            uint Lists[];
        };
        layout(binding = 2, r32ui) restrict uniform uimage2D PhysicalPoolTexture;

        PushConstant PushConstants
        {
            uvec4 Pool; // x = physical page count
        };

        void main()
        {
            const VsmPageEntry entry   = Entries[Lists[2u * Pool.x + gl_WorkGroupID.x]];
            const ivec2        local   = ivec2( gl_LocalInvocationID.xy );
            const ivec2        merged  = VsmPoolPageOrigin( entry.Physical ) + local;
            const ivec2        still   = VsmPoolStaticPageOrigin( entry.Physical, imageSize( PhysicalPoolTexture ).y ) + local;
            const bool         redraw  = ( entry.Flags & VSM_FLAG_DIRTY_STATIC ) != 0u;
            for ( int y = 0; y < int( VSM_PAGE_SIZE ); y += 16 )
            {
                for ( int x = 0; x < int( VSM_PAGE_SIZE ); x += 16 )
                {
                    const ivec2 offset = ivec2( x, y );
                    if ( redraw )
                    {
                        imageStore( PhysicalPoolTexture, merged + offset, uvec4( 0u ) );
                        imageStore( PhysicalPoolTexture, still + offset, uvec4( 0u ) );
                    }
                    else
                    {
                        imageStore( PhysicalPoolTexture, merged + offset, imageLoad( PhysicalPoolTexture, still + offset ) );
                    }
                }
            }
        }
    }
}

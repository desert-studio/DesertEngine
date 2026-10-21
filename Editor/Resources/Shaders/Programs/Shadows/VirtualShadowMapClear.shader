// DesertAsset {"Kind":"Shader","Guid":"8d42abddc8ce465db218b88154406ebc","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapClear"
{
    // "VSM: Clear Pages" (UE ClearPhysicalPages): one group per dirty page of the frame, dispatched indirectly from
    // "VSM: Clear Args". The page's 128 x 128 texels go to 0 = no occluder, the value the casters' atomic max then
    // raises.
    Compute
    {
        LocalSize(16, 16, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        ReadBuffer(1) VsmLists
        {
            uint u_Lists[];
        };
        layout(binding = 2, r32ui) restrict writeonly uniform uimage2D u_PhysicalPool;

        PushConstant PushConstants
        {
            uvec4 u_Pool; // x = physical page count
        };

        void main()
        {
            const uint  entry  = u_Lists[2u * u_Pool.x + gl_WorkGroupID.x];
            const ivec2 origin = VsmPoolPageOrigin( u_Entries[entry].Physical ) + ivec2( gl_LocalInvocationID.xy );
            for ( int y = 0; y < int( VSM_PAGE_SIZE ); y += 16 )
                for ( int x = 0; x < int( VSM_PAGE_SIZE ); x += 16 )
                    imageStore( u_PhysicalPool, origin + ivec2( x, y ), uvec4( 0u ) );
        }
    }
}

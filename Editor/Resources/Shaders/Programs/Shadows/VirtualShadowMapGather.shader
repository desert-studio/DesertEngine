// DesertAsset {"Kind":"Shader","Guid":"9b180e5ddf9b4807acb9c7df678bde81","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapGather"
{
    // "VSM: Gather Available Pages" (UE VirtualShadowMapPageManagement.usf, the available-page list). One thread per
    // physical page: a free page goes on the empty list, a page that holds a cached entry nobody requested this
    // frame on the cached list. "VSM: Allocate" takes the empty ones first, then the cached ones (whose entry it
    // unmaps): a cached page survives as long as the pool has room for this frame's requests.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        ReadBuffer(1) VsmPhysicalOwners
        {
            uint u_Owners[];
        };
        Buffer(2) VsmAllocator
        {
            uint  u_EmptyCount;
            uint  u_CachedCount;
            uint  u_AllocCursor;
            uint  u_DirtyCount;
            uint  u_Overflow;
            uint  u_Reserved0;
            uint  u_Reserved1;
            uint  u_Reserved2;
            uvec4 u_ClearArgs; // VkDispatchIndirectCommand of "VSM: Clear Pages"
        };
        Buffer(3) VsmLists
        {
            uint u_Lists[]; // [0, n) empty, [n, 2n) cached, [2n, 3n) dirty entries; n = physical page count
        };

        PushConstant PushConstants
        {
            uvec4 u_Pool; // x = physical page count
        };

        void main()
        {
            const uint page  = gl_GlobalInvocationID.x;
            const uint count = u_Pool.x;
            if ( page >= count )
                return;
            const uint owner = u_Owners[page];
            if ( owner == VSM_INVALID )
                u_Lists[atomicAdd( u_EmptyCount, 1u )] = page;
            else if ( ( u_Entries[owner].Flags & VSM_FLAG_REQUESTED ) == 0u )
                u_Lists[count + atomicAdd( u_CachedCount, 1u )] = page;
        }
    }
}

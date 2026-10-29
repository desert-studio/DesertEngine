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
            VsmPageEntry Entries[];
        };
        ReadBuffer(1) VsmPhysicalOwners
        {
            uint Owners[];
        };
        Buffer(2) VsmAllocator
        {
            uint  EmptyCount;
            uint  CachedCount;
            uint  AllocationCursor;
            uint  DirtyCount;
            uint  Overflow;
            uint  Reserved0;
            uint  Reserved1;
            uint  Reserved2;
            uvec4 ClearArgs; // VkDispatchIndirectCommand of "VSM: Clear Pages"
        };
        Buffer(3) VsmLists
        {
            uint Lists[]; // [0, n) empty, [n, 2n) cached, [2n, 3n) dirty entries; n = physical page count
        };

        PushConstant PushConstants
        {
            uvec4 Pool; // x = physical page count
        };

        void main()
        {
            const uint page  = gl_GlobalInvocationID.x;
            const uint count = Pool.x;
            if ( page >= count )
                return;
            const uint owner = Owners[page];
            if ( owner == VSM_INVALID )
                Lists[atomicAdd( EmptyCount, 1u )] = page;
            else if ( ( Entries[owner].Flags & VSM_FLAG_REQUESTED ) == 0u )
                Lists[count + atomicAdd( CachedCount, 1u )] = page;
        }
    }
}

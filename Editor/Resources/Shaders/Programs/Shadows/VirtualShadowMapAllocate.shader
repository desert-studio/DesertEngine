// DesertAsset {"Kind":"Shader","Guid":"15f4d874b5af4cbeb3777b38eb63fdff","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapAllocate"
{
    // "VSM: Allocate" (UE VirtualShadowMapPageManagement.usf AllocateNewPageMappings). One thread per page-table
    // entry: a requested entry with no physical page takes the next available one ("VSM: Gather Available Pages":
    // empty first, then cached - whose old entry is unmapped, it was not requested this frame); every requested
    // dirty entry goes on the dirty list, cleared by "VSM: Clear Pages" and drawn by the casters. A request the pool
    // cannot hold is counted in u_Overflow and stays unmapped: its receivers are shaded from a coarser level
    // ("VSM: Project" falls back) or lit.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame u_Frame;
            VsmLevel u_Levels[];
        };
        Buffer(1) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        Buffer(2) VsmPhysicalOwners
        {
            uint u_Owners[];
        };
        Buffer(3) VsmAllocator
        {
            uint  u_EmptyCount;
            uint  u_CachedCount;
            uint  u_AllocCursor;
            uint  u_DirtyCount;
            uint  u_Overflow;
            uint  u_Reserved0;
            uint  u_Reserved1;
            uint  u_Reserved2;
            uvec4 u_ClearArgs;
        };
        Buffer(4) VsmLists
        {
            uint u_Lists[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Pool; // x = physical page count
        };

        void main()
        {
            const uint level = gl_WorkGroupID.y;
            const uint local = gl_GlobalInvocationID.x;
            if ( local >= VSM_PAGES_PER_LEVEL )
                return;
            const uint index = level * VSM_PAGES_PER_LEVEL + local;
            uint       flags = u_Entries[index].Flags;
            if ( ( flags & VSM_FLAG_REQUESTED ) == 0u )
                return;

            const uint count = u_Pool.x;
            if ( ( flags & VSM_FLAG_ALLOCATED ) == 0u )
            {
                const uint taken = atomicAdd( u_AllocCursor, 1u );
                const uint empty = u_EmptyCount;
                if ( taken >= empty + u_CachedCount )
                {
                    atomicAdd( u_Overflow, 1u );
                    return;
                }
                const uint physical = taken < empty ? u_Lists[taken] : u_Lists[count + taken - empty];
                const uint previous = u_Owners[physical];
                if ( previous != VSM_INVALID )
                    u_Entries[previous].Flags = 0u; // a cached page nobody requested: its entry is unmapped
                u_Owners[physical] = index;
                const ivec2 world  = VsmWorldPageOfSlot( u_Levels[level].Window.xy, local );
                flags              = VSM_FLAG_ALLOCATED | VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED;
                u_Entries[index]   = VsmPageEntry( physical, flags, world.x, world.y );
            }
            if ( ( flags & VSM_FLAG_DIRTY ) != 0u )
                u_Lists[2u * count + atomicAdd( u_DirtyCount, 1u )] = index;
        }
    }
}

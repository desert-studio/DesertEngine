// DesertAsset {"Kind":"Shader","Guid":"15f4d874b5af4cbeb3777b38eb63fdff","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapAllocate"
{
    // "VSM: Allocate" (UE VirtualShadowMapPageManagement.usf AllocateNewPageMappings). One thread per page-table
    // entry: a requested entry with no physical page takes the next available one ("VSM: Gather Available Pages":
    // empty first, then cached - whose old entry is unmapped, it was not requested this frame); every requested
    // dirty entry goes on the dirty list, cleared by "VSM: Clear Pages" and drawn by the casters. A request the pool
    // cannot hold is counted in Overflow and stays unmapped: its receivers are shaded from a coarser level
    // ("VSM: Project" falls back) or lit.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        Buffer(1) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        Buffer(2) VsmPhysicalOwners
        {
            uint Owners[];
        };
        Buffer(3) VsmAllocator
        {
            uint  EmptyCount;
            uint  CachedCount;
            uint  AllocationCursor;
            uint  DirtyCount;
            uint  Overflow;
            uint  Reserved0;
            uint  Reserved1;
            uint  Reserved2;
            uvec4 ClearArgs;
        };
        Buffer(4) VsmLists
        {
            uint Lists[];
        };

        PushConstant PushConstants
        {
            uvec4 Pool; // x = physical page count
        };

        void main()
        {
            const uint level = gl_WorkGroupID.y;
            const uint local = gl_GlobalInvocationID.x;
            if ( local >= VSM_PAGES_PER_LEVEL )
                return;
            const uint index = level * VSM_PAGES_PER_LEVEL + local;
            uint       flags = Entries[index].Flags;
            if ( ( flags & VSM_FLAG_REQUESTED ) == 0u )
                return;

            const uint count = Pool.x;
            if ( ( flags & VSM_FLAG_ALLOCATED ) == 0u )
            {
                const uint taken = atomicAdd( AllocationCursor, 1u );
                const uint empty = EmptyCount;
                if ( taken >= empty + CachedCount )
                {
                    atomicAdd( Overflow, 1u );
                    return;
                }
                const uint physical = taken < empty ? Lists[taken] : Lists[count + taken - empty];
                const uint previous = Owners[physical];
                if ( previous != VSM_INVALID )
                    Entries[previous].Flags = 0u; // a cached page nobody requested: its entry is unmapped
                Owners[physical] = index;
                const ivec2 world  = VsmWorldPageOfSlot( Levels[level].Window.xy, local );
                flags              = VSM_FLAG_ALLOCATED | VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED;
                Entries[index]   = VsmPageEntry( physical, flags, world.x, world.y );
            }
            if ( ( flags & VSM_FLAG_DIRTY ) != 0u )
                Lists[2u * count + atomicAdd( DirtyCount, 1u )] = index;
        }
    }
}

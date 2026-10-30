// DesertAsset {"Kind":"Shader","Guid":"07601679d1ac4b239a341734d212ed6a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapClearArgs"
{
    // "VSM: Clear Args": one thread writes the indirect dispatch of "VSM: Clear Pages" - one group per dirty page
    // "VSM: Allocate" listed (UE: the GPU-sized dispatch of InitPhysicalMemory / ClearPhysicalPages).
    Compute
    {
        LocalSize(1, 1, 1);

        Buffer(0) VsmAllocator
        {
            uint  EmptyCount;
            uint  CachedCount;
            uint  AllocationCursor;
            uint  DirtyCount;
            uint  Overflow;
            uint  StaticRedrawn;  // of DirtyCount, the pages whose static layer is redrawn
            uint  MovableRedrawn; // of DirtyCount, the pages whose static layer is copied (the movers drawn alone)
            uint  Padding;
            uvec4 ClearArgs;
        };

        void main()
        {
            ClearArgs = uvec4( DirtyCount, 1u, 1u, 0u );
        }
    }
}

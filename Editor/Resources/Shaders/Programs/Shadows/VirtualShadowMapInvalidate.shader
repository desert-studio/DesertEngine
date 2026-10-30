// DesertAsset {"Kind":"Shader","Guid":"faeb9f9d85594d6caa34963fa32973fb","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapInvalidate"
{
    // "VSM: Invalidate" (UE VirtualShadowMapCacheManager: the cached page's validity against this frame's clipmap).
    // One thread per page-table entry of every level, first of the frame's page passes:
    //   - a page requested and dirty last frame was cleared and drawn then: it is clean now;
    //   - last frame's requests are dropped ("VSM: Mark" makes this frame's);
    //   - a mapped page whose slot now stands for another world page (the window panned past it), of a level reset
    //     this frame (its depth range moved) or of a map reset whole (the sun turned, the static casters changed,
    //     the pool was made) is released: its physical page goes back to the pool, its contents are not trusted.
    // The moved casters' pages are marked dirty after this ("VSM: Invalidate Casters").
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
            uint Owners[]; // per physical page: the page-table entry it holds, VSM_INVALID = free
        };

        PushConstant PushConstants
        {
            uvec4 Params; // x = 1: the whole map is reset this frame
        };

        void main()
        {
            const uint level = gl_WorkGroupID.y;
            const uint local = gl_GlobalInvocationID.x;
            const uint side  = Levels[level].Pages.y;
            if ( local >= side * side )
                return;
            const uint         index = Levels[level].Pages.x + local;
            const VsmPageEntry entry = Entries[index];
            uint               flags = entry.Flags;

            const uint drawn = VSM_FLAG_ALLOCATED | VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED;
            if ( ( flags & drawn ) == drawn )
                flags &= ~VSM_FLAG_DIRTY;
            flags &= ~( VSM_FLAG_REQUESTED | VSM_FLAG_MARK_KINDS );

            if ( ( flags & VSM_FLAG_ALLOCATED ) != 0u )
            {
                const ivec2 expected = VsmExpectedTag( Levels[level], local );
                const bool  stale    = Params.x != 0u || Levels[level].Window.w != 0 || entry.TagX != expected.x ||
                                   entry.TagY != expected.y;
                if ( stale )
                {
                    Owners[entry.Physical] = VSM_INVALID;
                    flags                    = 0u;
                }
            }
            Entries[index].Flags = flags;
        }
    }
}

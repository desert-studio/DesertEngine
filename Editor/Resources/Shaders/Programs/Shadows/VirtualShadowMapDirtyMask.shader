// DesertAsset {"Kind":"Shader","Guid":"07b21278bd674937849c1027d0610c27","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapDirtyMask"
{
    // "VSM: Dirty Mask" (UE: the page flags' hierarchy the per-page draw cull reads, VirtualShadowMapPageManagement /
    // CullPerPageDrawCommands). One thread per page-table entry of every level, after "VSM: Allocate": a page drawn this
    // frame (allocated, dirty, requested - the pages "VSM: Raster" writes, Common/VirtualShadowRaster.glslh) sets the bit
    // of its cell, VSM_MASK_CELL_PAGES x VSM_MASK_CELL_PAGES window pages, in its level's words. "VSM: Cull Casters"
    // then draws a caster into a level only where its footprint covers such a cell.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame u_Frame;
            VsmLevel u_Levels[];
        };
        ReadBuffer(1) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        Buffer(2) VsmDirtyMask
        {
            uint u_Mask[]; // level l, cell c: bit c % 32 of word l * VSM_MASK_WORDS_PER_LEVEL + c / 32
        };

        void main()
        {
            const uint level = gl_WorkGroupID.y;
            const uint local = gl_GlobalInvocationID.x;
            if ( local >= VSM_PAGES_PER_LEVEL )
                return;
            const uint drawn = VSM_FLAG_ALLOCATED | VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED;
            if ( ( u_Entries[level * VSM_PAGES_PER_LEVEL + local].Flags & drawn ) != drawn )
                return;
            // The slot's world page this frame, as a page of the level's window (0 .. 127 on each axis).
            const ivec2 window     = u_Levels[level].Window.xy;
            const ivec2 windowPage = VsmWorldPageOfSlot( window, local ) - window;
            const uvec2 cell       = uvec2( windowPage ) / VSM_MASK_CELL_PAGES;
            const uint  index      = cell.y * VSM_MASK_CELLS + cell.x;
            atomicOr( u_Mask[level * VSM_MASK_WORDS_PER_LEVEL + index / 32u], 1u << ( index % 32u ) );
        }
    }
}

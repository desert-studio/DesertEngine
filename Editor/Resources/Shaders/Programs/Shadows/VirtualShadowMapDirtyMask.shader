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
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        ReadBuffer(1) VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        Buffer(2) VsmDirtyMask
        {
            uint DirtyPageMask[]; // level l, cell c: bit c % 32 of word l * VSM_MASK_WORDS_PER_LEVEL + c / 32
        };
        // The level's pages this frame, for the host (VirtualShadowMapRenderer::GetLocalLightStats): x = requested,
        // y = rendered (requested and dirty: cleared and drawn), z = cached (requested, kept from an earlier frame).
        Buffer(3) VsmLevelStats
        {
            uvec4 LevelStats[];
        };

        void main()
        {
            const uint level = gl_WorkGroupID.y;
            const uint local = gl_GlobalInvocationID.x;
            const uint side  = Levels[level].Pages.y;
            if ( local >= side * side )
                return;
            const uint drawn = VSM_FLAG_ALLOCATED | VSM_FLAG_DIRTY | VSM_FLAG_REQUESTED;
            const uint flags = Entries[Levels[level].Pages.x + local].Flags;
            if ( ( flags & VSM_FLAG_REQUESTED ) != 0u )
            {
                atomicAdd( LevelStats[level].x, 1u );
                if ( ( flags & VSM_FLAG_ALLOCATED ) != 0u )
                {
                    if ( ( flags & VSM_FLAG_DIRTY ) != 0u )
                        atomicAdd( LevelStats[level].y, 1u );
                    else
                        atomicAdd( LevelStats[level].z, 1u );
                }
            }
            if ( ( flags & drawn ) != drawn )
                return;
            // The slot's world page this frame, as a page of the level's window (0 .. 127 on each axis).
            const ivec2 windowPage = VsmWindowPageOfSlot( Levels[level], local );
            const uvec2 cell       = uvec2( windowPage ) / VSM_MASK_CELL_PAGES;
            const uint  index      = cell.y * VSM_MASK_CELLS + cell.x;
            atomicOr( DirtyPageMask[level * VSM_MASK_WORDS_PER_LEVEL + index / 32u], 1u << ( index % 32u ) );
        }
    }
}

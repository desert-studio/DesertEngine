// DesertAsset {"Kind":"Shader","Guid":"dd04673cb49a468eb7c0ed6f7c6089d1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapInvalidateCasters"
{
    // "VSM: Invalidate Casters" (UE FVirtualShadowMapCacheManager::ProcessInvalidations, the per-primitive and
    // per-instance invalidation over the pages its bounds cover). One group per (world box, level): the box is a
    // changed caster's bounds this frame or last frame (where it stood: its old depth must go too), projected into the
    // level; every mapped page of the level's window under it is marked dirty, so it is drawn again when requested.
    // A still caster's box (the first Params.x boxes) marks the page's static layer too (VSM_FLAG_DIRTY_STATIC); a
    // mover's box only its merged layer on a sun level ("VSM: Clear Pages" copies the static layer back, the movers
    // are drawn over it), both on a local view (its raster draws every caster into both layers).
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
        ReadBuffer(2) VsmInvalidations
        {
            vec4 Boxes[]; // box b: [2b].xyz = world min cm, [2b + 1].xyz = world max cm
        };

        PushConstant PushConstants
        {
            uvec4 Params; // x = the first mover's box, y = the box count, z = boxes per z slice of the grid
        };

        void main()
        {
            const uint  box   = gl_WorkGroupID.z * Params.z + gl_WorkGroupID.x;
            const uint  level = gl_WorkGroupID.y;
            if ( box >= Params.y )
                return;
            const bool  still = box < Params.x || Levels[level].Pages.z == VSM_LEVEL_LOCAL;
            const uint  dirty = VSM_FLAG_DIRTY | ( still ? VSM_FLAG_DIRTY_STATIC : 0u );
            const vec3  lo    = Boxes[2u * box].xyz;
            const vec3  hi    = Boxes[2u * box + 1u].xyz;
            // The pages the box covers in this level (a clipmap level or a local light's mip: the motion mask).
            ivec2 lowPage;
            ivec2 highPage;
            float nearest;
            if ( !VsmLevelPageRect( Levels[level], lo, hi, lowPage, highPage, nearest ) )
                return;
            const ivec2 size  = highPage - lowPage + 1;
            const uint  count = uint( size.x * size.y );
            for ( uint k = gl_LocalInvocationID.x; k < count; k += 64u )
            {
                const ivec2 windowPage = lowPage + ivec2( int( k ) % size.x, int( k ) / size.x );
                const uint  index      = VsmLevelEntryIndex( Levels[level], level, windowPage );
                if ( ( Entries[index].Flags & VSM_FLAG_ALLOCATED ) != 0u )
                    atomicOr( Entries[index].Flags, dirty );
            }
        }
    }
}

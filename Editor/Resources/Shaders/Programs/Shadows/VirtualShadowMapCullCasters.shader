// DesertAsset {"Kind":"Shader","Guid":"4da042c34d4c42e68256c195d69a1fb3","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapCullCasters"
{
    // "VSM: Cull Casters" of one clipmap level (UE 5.8 Engine/Shaders/Private/VirtualShadowMaps/
    // VirtualShadowMapBuildPerPageDrawCommands.usf, CullPerPageDrawCommands: an instance is drawn into a virtual shadow
    // map only where its bounds cover a page that is drawn this frame; the surviving instances are compacted into the
    // draw's instance list and its indirect instance count), adapted: the page test is against "VSM: Dirty Mask"'s cells
    // of 8 x 8 pages instead of UE's page-flag mips, and a draw's records are VkDrawIndexedIndirectCommand
    // (Graphic/MeshDrawArguments.hpp: one per drawn submesh, every one counting the same instances).
    //
    // One thread per instance. A surviving instance takes the next slot of its draw (the first record's instance count,
    // counted up from 0), writes the instance index its vertex stage fetches to the draw's remap slot (the records'
    // first instance is the draw's RemapBase, so gl_InstanceIndex names the slot - Mesh/Surface/Vertex_Instanced.glslh
    // reads InstanceRemap in the VirtualShadowDepth cell) and counts itself into the draw's other records. A level
    // with no drawn page leaves every count at 0: nothing of it is rasterised.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        struct VsmCullInstance
        {
            vec4  Min;
            vec4  Max;
            uvec4 Draw; // x = its draw, y = the instance index its vertex stage fetches
        };

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        ReadBuffer(1) VsmDirtyMask
        {
            uint DirtyPageMask[];
        };
        ReadBuffer(2) VsmCullInstances
        {
            VsmCullInstance Instances[];
        };
        ReadBuffer(3) VsmCullDraws
        {
            uvec4 Draws[]; // x = first record, y = records, z = remap base
        };
        Buffer(4) VsmCasterArguments
        {
            uint Arguments[]; // VSM_DRAW_ARGUMENT_WORDS per record, the instance count at word 1
        };
        Buffer(5) VsmInstanceRemap
        {
            uint Remap[];
        };

        PushConstant PushConstants
        {
            uvec4 Params; // x = level, y = instance count, z = threads per row of groups
        };

        bool CoversDrawnCell( uint level, ivec2 lowCell, ivec2 highCell )
        {
            for ( int y = lowCell.y; y <= highCell.y; ++y )
            {
                for ( int x = lowCell.x; x <= highCell.x; ++x )
                {
                    const uint index = uint( y ) * VSM_MASK_CELLS + uint( x );
                    if ( ( DirtyPageMask[level * VSM_MASK_WORDS_PER_LEVEL + index / 32u] & ( 1u << ( index % 32u ) ) ) != 0u )
                        return true;
                }
            }
            return false;
        }

        void main()
        {
            const uint i = gl_GlobalInvocationID.y * Params.z + gl_GlobalInvocationID.x;
            if ( i >= Params.y )
                return;
            const uint            level    = Params.x;
            const VsmCullInstance instance = Instances[i];
            // The box's footprint in the level's pages (Common/VirtualShadowMap.glslh VsmLevelPageRect: a sun level's
            // window or a local view's perspective map), as "VSM: Dirty Mask" cells. Nearer the sun than the level's
            // range still draws (depth clamp, "VSM: Raster"); wholly past its far end does not.
            ivec2 lowPage;
            ivec2 highPage;
            float nearest;
            if ( !VsmLevelPageRect( Levels[level], instance.Min.xyz, instance.Max.xyz, lowPage, highPage, nearest ) ||
                 !CoversDrawnCell( level, lowPage / int( VSM_MASK_CELL_PAGES ), highPage / int( VSM_MASK_CELL_PAGES ) ) )
                return;
            const uvec4 draw = Draws[instance.Draw.x];
            const uint  slot = atomicAdd( Arguments[draw.x * VSM_DRAW_ARGUMENT_WORDS + 1u], 1u );
            for ( uint r = 1u; r < draw.y; ++r )
                atomicAdd( Arguments[( draw.x + r ) * VSM_DRAW_ARGUMENT_WORDS + 1u], 1u );
            Remap[draw.z + slot] = instance.Draw.y;
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"8d1f4b7e2a9c4e63b5f0a7d2c4e81b96","Versions":{"SHDR":1},"Dependencies":[]}
// "Grass: Visibility Classify" (Engine/Graphic/VirtualGeometry/VisibilityBuffer.cpp; UE 5 Nanite's material tile
// classification, r.Nanite.MaterialCulling): after the last raster into the view's visibility buffer (phase 2's,
// which appends to phase 1's words), one group per kVisTileSize x kVisTileSize tile reads its pixels' words and
// writes the tile's record (Common/VisibilityTiles.glslh: the smallest and largest row its pixels hold and the
// 64-bucket row mask between them) and, when it holds a drawn word, appends itself to the held tiles' list. Group 0
// writes record 0 of the resolve's argument table (3 indices) and the last group done its instance count: two per
// held tile, the triangles each section's resolve draws (Vertex_GrassBlade.glslh /
// Vertex_Instanced.glslh's resolve branch collapses the tiles that miss its rows). The buffer is read as 32-bit
// words (the layout VisibilityClear.shader writes: header = words 0..3, pixel p = words 4 + 2p (payload),
// 4 + 2p + 1 (depth)), so this program needs no 64-bit type.
Shader "VisibilityClassify"
{
    Compute
    {
        LocalSize(16, 16, 1);

        #include <Common/VisibilityTiles.glslh>

        Buffer(0) Visibility
        {
            uint Words[];
        };

        Buffer(1) ResolveArgs
        {
            uint Args[];
        };

        PushConstant PushConstants
        {
            uvec4 Extent; // x = width, y = height (pixels), z = tile columns, w = tile rows
        };

        shared uint s_Smallest;
        shared uint s_Largest;
        shared uint s_Mask[2];

        void main()
        {
            const bool leader = gl_LocalInvocationIndex == 0u;
            if (leader)
            {
                s_Smallest = kVisTileEmpty;
                s_Largest  = 0u;
                s_Mask[0]  = 0u;
                s_Mask[1]  = 0u;
            }
            barrier();

            const uvec2 pixel = gl_GlobalInvocationID.xy;
            bool        drawn = false;
            uint        row   = 0u;
            if (pixel.x < Extent.x && pixel.y < Extent.y)
            {
                const uint word    = 4u + 2u * (pixel.y * Extent.x + pixel.x);
                const uint payload = Words[word];
                // A word of 0 is nothing drawn (Common/VisibilityBuffer.glslh); a drawn word has the depth's bits.
                drawn = Words[word + 1u] != 0u || payload != 0u;
                row   = payload >> 7u; // kVisTriangleBits
            }
            if (drawn)
            {
                atomicMin(s_Smallest, row);
                atomicMax(s_Largest, row);
            }
            barrier();

            if (drawn)
            {
                const uint bucket = DesertVisibilityTileBucket(row, s_Smallest, s_Largest - s_Smallest + 1u);
                atomicOr(s_Mask[bucket >> 5u], 1u << (bucket & 31u));
            }
            barrier();

            if (!leader)
                return;
            const uint tiles = Extent.z * Extent.w;
            const uint tile  = gl_WorkGroupID.y * Extent.z + gl_WorkGroupID.x;
            const uint at    = DesertVisibilityTileWord(Extent.xy, tile);
            Words[at]        = s_Smallest;
            Words[at + 1u]   = s_Largest;
            Words[at + 2u]   = s_Mask[0];
            Words[at + 3u]   = s_Mask[1];
            // FPS-GRASS-RESOLVE-4: a tile holding any drawn word joins the held tiles' list (header word 2 counts
            // it; Common/VisibilityTiles.glslh), the slot bounded so a second classify of one clear never writes
            // past the list.
            if (s_Smallest != kVisTileEmpty)
            {
                const uint slot = atomicAdd(Words[2], 1u);
                if (slot < tiles)
                    Words[DesertVisibilityTileListWord(Extent.xy, tiles, slot)] = tile;
            }
            // The last group done (header word 3) writes the draw's instance count from the final list length:
            // every group added to word 2 before it added to word 3, and both are atomics of one buffer.
            memoryBarrierBuffer();
            const uint done = atomicAdd(Words[3], 1u);
            if (done + 1u == tiles)
                Args[5] = 2u * min(atomicAdd(Words[2], 0u), tiles);
            if (tile == 0u)
            {
                // Record 0 (VkDrawIndexedIndirectCommand, after the 16-byte header: GpuInstanceArgsOffset( 0 ));
                // its instance count (Args[5]) is the last group's.
                Args[4] = 3u;
                Args[6] = 0u;
                Args[7] = 0u;
                Args[8] = 0u;
            }
        }
    }
}

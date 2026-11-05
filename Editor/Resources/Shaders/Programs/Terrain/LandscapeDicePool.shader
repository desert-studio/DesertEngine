// DesertAsset {"Kind":"Shader","Guid":"67b3969e19474567a4301d63550b82b3","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LandscapeDicePool"
{
    Compute
    {
        // "Landscape: Dice Pool" (TerrainRenderer::AddDicePoolPass). UE: Nanite displaces each vertex of a cluster
        // ONCE and its triangles index it; here the displaced landscape was evaluated per triangle CORNER (a
        // vertexless draw, six corners a sub-cell, so every sub-vertex ~6 times) and again in every shadow view.
        // This pass evaluates each diced tile's sub-vertices once per frame - (dice + 1)^2 a cell, laid out by the
        // camera's rings (LandscapeDisplacement.glslh LandscapeDecodePoolVertex) - into the dice pool, and every
        // view's draw reads them by index (LandscapeDicePoolIndex): the camera's G-buffer / forward pass, the
        // cascades (whose coarser dice is a subset of the camera's vertices) and the virtual shadow map.
        //
        // Output: two vec4 a vertex - (world position, local x), (base normal, local y) - from the row's
        // DicePool.x on. The evaluation is LandscapeDicedPoint, the one the vertex stage ran before the pool.
        //
        // TWO DISPATCHES A TILE (TERRAIN-COST-2): a sub-vertex is the barycentric point of its cell's triangle,
        // whose three corners are LandscapeEvaluateVertex's (the section LOD's nine-entry table, the morph, ten
        // heightmap fetches with the gradient) - evaluated by every sub-vertex itself, a cell's four corners were
        // evaluated (dice + 1)^2 x 3 times (867 on a cell diced 16). Tile.w = 0 evaluates each node of the fade's
        // square once into LandscapeDicePoolCorners (TerrainBatch.hpp LandscapeDicePoolCornerCount, from Tile.z);
        // Tile.w = 1, after it (TerrainRenderer::AddDicePoolPass: the next node), interpolates and displaces the
        // sub-vertices from those nodes - the same values, so the same vertices.
        LocalSize(64, 1, 1);

        #include <Common/TerrainInstance.glslh>
        // The camera's TerrainInstance rows of the diced tiles (TerrainRenderer::PrepareFrame), one per dispatch.
        ReadBuffer(0) LandscapeDicePoolRows
        {
            TerrainInstance Terrains[];
        };
        // Shared sampler states, bound by the pass by name: the heightmap is read only by texelFetch, the
        // weightmap bilinearly (as the material's clamped state), the layer heights tiled with their mips.
        Uniform sampler PointClampSampler;
        Uniform sampler LinearClampSampler;
        Uniform sampler LinearRepeatSampler;
        Uniform(1) texture2D HeightmapTexture;
        #define HeightmapTexture sampler2D(HeightmapTexture, PointClampSampler)
        Uniform(2) texture2D WeightmapTexture;
        #define WeightmapTexture sampler2D(WeightmapTexture, LinearClampSampler)
        Uniform(3) texture2DArray LayerHeightTexture;
        #define LayerHeightTexture sampler2DArray(LayerHeightTexture, LinearRepeatSampler)

        Buffer(4) LandscapeDicePoolVertices
        {
            vec4 PoolVertices[];
        };
        // The diced tiles' corner nodes: two vec4 a node - (world position, continuous LOD), (gradient, local).
        Buffer(5) LandscapeDicePoolCorners
        {
            vec4 PoolCorners[];
        };

        PushConstant PushConstants
        {
            // x = the tile's row, y = this dispatch's items (its corner nodes, or its pool vertices), z = the row's
            // first corner node, w = the dispatch: 0 = the corner nodes, 1 = the pool vertices
            uvec4 Tile;
        };

        #define CurrentTerrain Terrains[Tile.x]

        #include <Common/LandscapeHeight.glslh>
        #include <Common/LandscapeLod.glslh>
        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/LandscapeVertexEval.glslh>
        #include <Programs/Terrain/LandscapeDisplacement.glslh>

        // The tile's grid cells a side (its grid LOD's; a pooled row's is LOD 0).
        int LandscapeDicePoolCells()
        {
            int iquads = int( CurrentTerrain.LandscapeTile.z + 0.5 );
            int stride = LandscapeLodStride( int( CurrentTerrain.Params.w + 0.5 ) );
            return ( iquads + stride - 1 ) / stride;
        }

        LandscapeDicedCorner LandscapeDicedCornerOf( int cellX, int cellZ )
        {
            // The fade's square (ring 0's rectangle: every pooled cell is in it), its nodes row-major.
            ivec4 r = LandscapeDicePoolRingRect( 0, LandscapeDicePoolCells() );
            uint  p = ( Tile.z + uint( ( cellZ - r.y ) * ( r.z - r.x + 1 ) + cellX - r.x ) ) * 2u;
            vec4 pa = PoolCorners[p];
            vec4 pb = PoolCorners[p + 1u];
            LandscapeDicedCorner c;
            c.WorldPos = pa.xyz;
            c.Lod      = pa.w;
            c.Gradient = pb.xy;
            c.Local    = pb.zw;
            return c;
        }

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= Tile.y )
                return;
            int iquads = int( CurrentTerrain.LandscapeTile.z + 0.5 );
            int stride = LandscapeLodStride( int( CurrentTerrain.Params.w + 0.5 ) );
            int cells  = LandscapeDicePoolCells();
            if ( Tile.w == 0u )
            {
                // Node (x0 + i % pitch, z0 + i / pitch) of the fade's square: the corner of the cells around it.
                ivec4 r     = LandscapeDicePoolRingRect( 0, cells );
                int   pitch = r.z - r.x + 1;
                int   nz    = int( i ) / pitch;
                int   nx    = int( i ) - nz * pitch;
                LandscapeDicedCorner c = LandscapeDicedCornerAt( LandscapeCellCorner( r.x + nx, stride, iquads ),
                                                                 LandscapeCellCorner( r.y + nz, stride, iquads ) );
                uint q = ( Tile.z + i ) * 2u;
                PoolCorners[q]      = vec4( c.WorldPos, c.Lod );
                PoolCorners[q + 1u] = vec4( c.Gradient, c.Local );
                return;
            }
            LandscapeDrawVertex d;
            if ( !LandscapeDecodePoolVertex( int( i ), cells, d ) )
                return;
            LandscapeDisplacedVertex v = LandscapeDicedPoint( d );
            uint p = ( uint( CurrentTerrain.DicePool.x + 0.5 ) + i ) * 2u;
            PoolVertices[p]      = vec4( v.WorldPos, v.Local.x );
            PoolVertices[p + 1u] = vec4( v.Normal, v.Local.y );
        }
    }
}

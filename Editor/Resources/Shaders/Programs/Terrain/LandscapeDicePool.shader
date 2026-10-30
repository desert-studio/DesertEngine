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

        PushConstant PushConstants
        {
            uvec4 Tile; // x = the tile's row, y = its pool vertices
        };

        #define CurrentTerrain Terrains[Tile.x]

        #include <Common/LandscapeHeight.glslh>
        #include <Common/LandscapeLod.glslh>
        #include <Common/LandscapeWeights.glslh>
        #include <Programs/Terrain/LandscapeVertexEval.glslh>
        #include <Programs/Terrain/LandscapeDisplacement.glslh>

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= Tile.y )
                return;
            int iquads = int( CurrentTerrain.LandscapeTile.z + 0.5 );
            int stride = LandscapeLodStride( int( CurrentTerrain.Params.w + 0.5 ) );
            int cells  = ( iquads + stride - 1 ) / stride;
            LandscapeDrawVertex d;
            if ( !LandscapeDecodePoolVertex( int( i ), cells, d ) )
                return;
            LandscapeDisplacedVertex v = LandscapeDicedPoint( d, stride, iquads );
            uint p = ( uint( CurrentTerrain.DicePool.x + 0.5 ) + i ) * 2u;
            PoolVertices[p]      = vec4( v.WorldPos, v.Local.x );
            PoolVertices[p + 1u] = vec4( v.Normal, v.Local.y );
        }
    }
}

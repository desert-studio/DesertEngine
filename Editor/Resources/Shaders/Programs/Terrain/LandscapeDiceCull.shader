// DesertAsset {"Kind":"Shader","Guid":"c4f1d82a9b3e4f6d8a17e05b2c9d6f31","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LandscapeDiceCull"
{
    Compute
    {
        // "Landscape: Dice Cull" (TerrainRenderer::AddDiceCullPass; UE Nanite: every cluster is culled by the
        // frustum and last frame's furthest HZB into a compact list before it is rasterised). One lane per cell of
        // a diced camera row's fade square (its every diced cell, LandscapeDiceRings.glslh): the cell's box - its
        // four corner nodes from the dice pool (LandscapeDicePool.shader Tile.w = 0; every sub-vertex is a
        // barycentric point of them) widened by the displacement's reach along any normal (Bounds.x) - against the
        // camera rasters' clip space and, when Hzb.z != 0, last frame's furthest HZB (Common/GpuCullTests.glslh,
        // the instance cull's rule). A kept cell is appended to its ring level's list (LandscapeDiceCells from
        // DicePool.z + level x Tile.y) and its dice^2 x 6 vertices to the level's indirect draw (record
        // Tile.w + level: vertexCount, instanceCount, firstVertex, firstInstance), which the camera's rasters draw
        // (LandscapeDisplacement.glslh LandscapeDecodeCulledVertex). Conservative: a box only grows, so no
        // visible sub-triangle is dropped; the casters keep the procedural decode of every cell.
        LocalSize(64, 1, 1);

        #include <Common/TerrainInstance.glslh>
        // The camera rows of the culled tiles (TerrainRenderer::QueueDiceCull), DicePool.z set.
        ReadBuffer(0) LandscapeDiceCullRows
        {
            TerrainInstance Terrains[];
        };
        // The dice pool's corner nodes: two vec4 a node - (world position, continuous LOD), (gradient, local).
        ReadBuffer(1) LandscapeDicePoolCorners
        {
            vec4 PoolCorners[];
        };
        Buffer(2) LandscapeDiceCells
        {
            uint DiceCells[];
        };
        Buffer(3) LandscapeDiceCullArgs
        {
            uint CullArgs[];
        };
        // The frame's two matrices (a buffer: the push block stays within the 128 bytes every device has).
        ReadBuffer(5) LandscapeDiceCullView
        {
            mat4 ClipFromWorld;     // the camera rasters' clip-from-world (jittered: what they draw through)
            mat4 HzbViewProjection; // unjittered, what LandscapeDiceHzbTexture was built through
        };
        Uniform sampler PointClampSampler;
        Uniform(4) texture2D LandscapeDiceHzbTexture;

        PushConstant PushConstants
        {
            uvec4 Hzb; // xy = the HZB's mip-0 size, z = its mip count (0: the frustum alone)
            // x = the row, y = the fade square's cells (the lanes; each level's list stride), z = the row's first
            // corner node, w = its first indirect record
            uvec4 Tile;
            vec4  Bounds; // x = the displacement's reach (cm): magnitude x max(|Center|, |1 - Center|)
        };

        #define CurrentTerrain Terrains[Tile.x]

        #include <Common/GpuCullTests.glslh>
        #include <Programs/Terrain/LandscapeDiceRings.glslh>

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= Tile.y )
                return;
            // A diced row's grid is LOD 0 (TerrainRenderer ApplyLandscapeDisplacement): its cells are its quads.
            int   cells = int( CurrentTerrain.LandscapeTile.z + 0.5 );
            ivec4 r     = LandscapeDiceRingRect( 0, cells );
            int   pitch = r.z - r.x;
            if ( pitch <= 0 )
                return;
            int z    = r.y + int( i ) / pitch;
            int x    = r.x + int( i ) - ( z - r.y ) * pitch;
            int dice = LandscapeCellDice( x, z );
            if ( dice <= 0 )
                return;

            // The cell's four corner nodes of the pool's square (row-major, LandscapeDicePool.shader Tile.w = 0).
            ivec4 n      = LandscapeDicePoolRingRect( 0, cells );
            int   nPitch = n.z - n.x + 1;
            vec3  lo     = vec3( 1.0e30 );
            vec3  hi     = vec3( -1.0e30 );
            for ( int c = 0; c < 4; ++c )
            {
                int  nx = x + ( c & 1 ) - n.x;
                int  nz = z + ( c >> 1 ) - n.y;
                vec3 p  = PoolCorners[( Tile.z + uint( nz * nPitch + nx ) ) * 2u].xyz;
                lo      = min( lo, p );
                hi      = max( hi, p );
            }
            lo -= vec3( Bounds.x );
            hi += vec3( Bounds.x );

            // Clip space: outside when every corner is past one side plane, or behind the eye (w <= 0). The far
            // plane is not tested (reversed infinite Z).
            uint outside = 31u;
            for ( uint c = 0u; c < 8u; ++c )
            {
                vec3 f      = vec3( float( c & 1u ), float( ( c >> 1u ) & 1u ), float( ( c >> 2u ) & 1u ) );
                vec4 p      = ClipFromWorld * vec4( mix( lo, hi, f ), 1.0 );
                uint code   = 0u;
                if ( p.x < -p.w )
                    code |= 1u;
                if ( p.x > p.w )
                    code |= 2u;
                if ( p.y < -p.w )
                    code |= 4u;
                if ( p.y > p.w )
                    code |= 8u;
                if ( p.w <= 0.0 )
                    code |= 16u;
                outside &= code;
            }
            if ( outside != 0u )
                return;
            if ( Hzb.z != 0u )
            {
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                if ( GpuCull_IsOccludedByHzb( LandscapeDiceHzbTexture, PointClampSampler, view, HzbViewProjection,
                                              lo, hi ) )
                    return;
            }

            int  level = findLSB( dice );
            uint per   = uint( dice * dice * 6 );
            uint at    = ( Tile.w + uint( level ) ) * 4u;
            uint slot  = atomicAdd( CullArgs[at], per ) / per;
            // FPS-DEVLOST9 (ClusterCut HwDraw's rule): a level's list holds Tile.y cells; an append past it is taken
            // back, so the draw's vertexCount stays inside the cells written and the list never runs into the next
            // level's or row's range.
            if ( slot >= Tile.y )
            {
                atomicAdd( CullArgs[at], 0u - per );
                return;
            }
            uint first = uint( CurrentTerrain.DicePool.z + 0.5 );
            DiceCells[first + uint( level ) * Tile.y + slot] = uint( x ) | ( uint( z ) << 16u );
        }
    }
}

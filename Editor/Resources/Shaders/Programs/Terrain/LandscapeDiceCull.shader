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
        //
        // THE CELL'S OWN DICE (FPS-TERRAIN-LOD-2; UE Nanite: a cluster's triangles span DicingRate pixels where IT
        // is): the rings dice a cell by the area a flat cell has at the worst height of the landscape's slab near
        // the eye; a kept cell is diced by its own corners' projected area instead - the largest power of two whose
        // sub-cell side keeps Bounds.w pixels, at most its ring's dice (the pool holds no finer vertex), and at
        // least each of its edge factors. An edge factor is min of the two cells' own dice inside the tile (each
        // lane computes its four neighbours' dice from the same corner nodes, so both cells of an edge agree bit
        // for bit), and across the tile's border min of the two ring dice (the neighbour tile's lane, or its
        // procedural decode, sees only the shared ring integers there; a border cell keeps at least that). The
        // list entry's second word carries the four factors (LandscapeDisplacement.glslh LandscapeEdgeDice).
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
            // x = the displacement's reach (cm): magnitude x max(|Center|, |1 - Center|); yz = half the camera
            // rasters' extent (pixels: clip xy / w to pixels); w = the dicing rate (pixels a sub-cell side)
            vec4  Bounds;
        };

        #define CurrentTerrain Terrains[Tile.x]

        #include <Common/GpuCullTests.glslh>
        #include <Programs/Terrain/LandscapeDiceRings.glslh>

        // The pool's corner nodes around the lane's cell (x, z): the 4 x 4 nodes x - 1 .. x + 2, z - 1 .. z + 2 in
        // pixels; Valid false where a node is outside the pool's square or at / behind the eye.
        vec2 WindowPixel[16];
        bool WindowValid[16];

        void LoadWindow( int x, int z, ivec4 n )
        {
            int nPitch = n.z - n.x + 1;
            for ( int k = 0; k < 16; ++k )
            {
                int nx         = x - 1 + ( k & 3 );
                int nz         = z - 1 + ( k >> 2 );
                WindowValid[k] = false;
                WindowPixel[k] = vec2( 0.0 );
                if ( nx < n.x || nx > n.z || nz < n.y || nz > n.w )
                    continue;
                vec3 p = PoolCorners[( Tile.z + uint( ( nz - n.y ) * nPitch + nx - n.x ) ) * 2u].xyz;
                vec4 c = ClipFromWorld * vec4( p, 1.0 );
                if ( c.w <= 1.0 )
                    continue;
                WindowValid[k] = true;
                WindowPixel[k] = c.xy / c.w * Bounds.yz;
            }
        }

        // Cell (@p cx, @p cz) of the tile's @p cells a side, at window offset (@p ox, @p oz) from the lane's cell
        // (-1..1): its own dice. Its ring dice where it is undiced or 1, where a corner is unknown, or past it.
        int CellOwnDice( int cx, int cz, int ox, int oz, int cells )
        {
            int ring = LandscapeCellDice( cx, cz );
            if ( ring <= 1 )
                return ring;
            int k00 = ( oz + 1 ) * 4 + ox + 1;
            int k10 = k00 + 1;
            int k01 = k00 + 4;
            int k11 = k00 + 5;
            if ( !( WindowValid[k00] && WindowValid[k10] && WindowValid[k01] && WindowValid[k11] ) )
                return ring;
            // The cell's two triangles (00, 10, 11) and (00, 11, 01), each by its own area: a quad folded at the
            // silhouette is not credited with the difference.
            vec2  a    = WindowPixel[k00];
            vec2  b    = WindowPixel[k10] - a;
            vec2  c    = WindowPixel[k11] - a;
            vec2  d    = WindowPixel[k01] - a;
            float area = 0.5 * ( abs( b.x * c.y - b.y * c.x ) + abs( c.x * d.y - c.y * d.x ) );
            float side = min( sqrt( area ) / max( Bounds.w, 1.0e-3 ), 16.0 );
            int   own  = side >= 1.0 ? 1 << findMSB( int( side ) ) : 1;
            // A side on the tile's border stitches to min of the two ring dice: the cell keeps at least that.
            if ( cz == 0 )
                own = max( own, min( ring, max( LandscapeCellDice( cx, cz - 1 ), 1 ) ) );
            if ( cz == cells - 1 )
                own = max( own, min( ring, max( LandscapeCellDice( cx, cz + 1 ), 1 ) ) );
            if ( cx == 0 )
                own = max( own, min( ring, max( LandscapeCellDice( cx - 1, cz ), 1 ) ) );
            if ( cx == cells - 1 )
                own = max( own, min( ring, max( LandscapeCellDice( cx + 1, cz ), 1 ) ) );
            return min( own, ring );
        }

        // The edge factor of the lane's cell (@p x, @p z), own dice @p dice, with neighbour (@p x + @p ox,
        // @p z + @p oz).
        int EdgeFactor( int x, int z, int dice, int ox, int oz, int cells )
        {
            int nx = x + ox;
            int nz = z + oz;
            if ( nx < 0 || nz < 0 || nx >= cells || nz >= cells )
                return min( LandscapeCellDice( x, z ), max( LandscapeCellDice( nx, nz ), 1 ) );
            return min( dice, max( CellOwnDice( nx, nz, ox, oz, cells ), 1 ) );
        }

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
            int ring = LandscapeCellDice( x, z );
            if ( ring <= 0 )
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

            // The cell's own dice and its edge factors (z-1, z+1, x-1, x+1), log2 three bits each.
            int  dice  = ring;
            uint edges = 0u;
            if ( ring > 1 )
            {
                LoadWindow( x, z, n );
                dice  = CellOwnDice( x, z, 0, 0, cells );
                edges = uint( findLSB( EdgeFactor( x, z, dice, 0, -1, cells ) ) ) |
                        ( uint( findLSB( EdgeFactor( x, z, dice, 0, 1, cells ) ) ) << 3u ) |
                        ( uint( findLSB( EdgeFactor( x, z, dice, -1, 0, cells ) ) ) << 6u ) |
                        ( uint( findLSB( EdgeFactor( x, z, dice, 1, 0, cells ) ) ) << 9u );
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
            uint entry = ( first + uint( level ) * Tile.y + slot ) * 2u;
            DiceCells[entry]      = uint( x ) | ( uint( z ) << 16u );
            DiceCells[entry + 1u] = edges;
        }
    }
}

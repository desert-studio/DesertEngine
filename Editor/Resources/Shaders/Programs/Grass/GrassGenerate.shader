// DesertAsset {"Kind":"Shader","Guid":"bfb6f16ce052412cb6dc16e913525849","Versions":{"SHDR":1},"Dependencies":[]}
// Landscape grass (GRASS-GPU; UE: ALandscapeProxy::UpdateGrass + FAsyncGrassBuilder::Build, LandscapeGrass.cpp:
// 2378-2520, its grid branch, ported to one thread per grid cell). The grid is WORLD-aligned (cell = sqrt(1e6 /
// Density) cm), so a cell grows the same instance from every camera position; its seed is a hash of the cell.
// GRASS-TILE-1, BUILT ONCE PER WORLD TILE, CULLED PER VIEW (UE LandscapeGrass.cpp: FAsyncGrassBuilder builds a
// component's instances once into its HISM, grass.MaxCreatePerFrame budgets the builds, and every view only culls,
// LODs and fades the HISM's instances): the grid is cut into world tiles of kTileCells x kTileCells cells, and a
// tile's instances - everything about an instance the view does not change - are built once into the variety's
// tile pool (TilePool, pages of kTilePageRecords records handed out by TileWords' free-page stack) and kept while
// the tile stays within the cull radius (GrassRenderer VarietyGpu::Tiles). The build (Count, Allocate, Write) keeps
// a cell's instance when its bilinear layer weight lies in AllowedDensityRange and beats the cell's random draw
// (UE's RandomStream.GetFraction), its patches let it grow, and its fade draw beats the field's share at the nearest
// the tile is built for (TileEntries' w): the thinning below is nested, so an instance dropped there is dropped at
// every farther view - and the renderer rebuilds a tile the eye comes nearer than that.
// THE VIEW LAW (GRASS-VIEW-LAW-1; UE landscape grass: a component's instances are built once, every view culls,
// LODs and fades them per frame) is not here: the build writes each cluster's chunk (WriteTileChunk) and
// GrassCull.shader applies the law per view and frame to the records of the chunks a view meets (GrassViewLaw:
// the distance thinning, the screen-width floor, the far LOD, the blade's widening and lowering, a mesh's
// level and shrink), writes the instance into its row of Instances (REGRESS-CACHE-1: a row of the lists'
// regions, Common/GrassVarietyRows.glslh THE ROWS) and appends that row to the camera's, the cascades' and the
// full list (FullIds, the union RT and the list-less draws read). Turning or stepping the view rebuilds nothing;
// only a tile entering the radius or a nearer view than it was built for builds.
Shader "GrassGenerate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GrassFieldShare.glslh>
        #include <Common/GrassVarietyRows.glslh>

        ReadBuffer(0) Field
        {
            uint FieldData[]; // GrassFieldLayout (LandscapeGrassField.hpp)
        };
        #define GRASS_FIELD_WORD( i ) FieldData[i]
        #include <Common/GrassTileRecord.glslh>


        ReadBuffer(1) Params
        {
            GrassParams GrassParamsData[];
        };

        // GRASS-VIEW-LAW-1: the build's chunks, one per (tile slot, cluster) (Common/GrassVarietyRows.glslh, the
        // chunk contract): Write writes a built cluster's, Free empties a freed slot's; the view law reads them.
        // Word 2 of the header = the chunks it has room for (GrassRenderer VarietyGpu::Chunks, made with the pool).
        Buffer(8) Chunks
        {
            uint GrassChunkData[];
        };

        // GRASS-TILE-1, THE TILE POOL: kTilePoolWords uvec4 per record (TileRecord: StoreRecord, Common/GrassTileRecord.glslh GrassUnpackRecord), in
        // pages of kTilePageRecords records; a tile holds up to kTilePages of them (its TileWords header).
        // GRASS-VRAM-BUDGET-1: the binding is the pool's segment table (Common/GrassVarietyRows.glslh, THE POOL'S
        // SEGMENTS): per segment of kGrassTileSegmentPages pages the device address of its buffer, which StoreRecord
        // writes through.
        ReadBuffer(10) TilePool
        {
            uvec2 TileSegmentData[];
        };

        // The pool's bookkeeping: [0] the free pages' count, [1, 1 + Pool.x) the free-page stack (room for every page
        // the pool can grow to), then per tile slot
        // kTileHeaderWords: the build's candidates, the pages held, 0, 0, the candidates' root box (min xyz, max xyz,
        // OrderedBits), 0 x 6, the pages held (kTilePages words from kTilePageWord), then its clusters' (from
        // kTileClusterWord: count, first record, root box).
        Buffer(11) TileWords
        {
            uint TileWordsData[];
        };

        // This frame's tile table (GrassRenderer VarietyGpu::TileTable): [0] x = the pool's generation (TileFeedback's
        // mark), then each mode's entries from Mode.y - Free: x = slot; Count / Write: x = slot, y, z = the tile
        // (world tile index), w = the nearest the tile is built for (float bits, cm); Cull: x = slot.
        ReadBuffer(12) Tiles
        {
            uvec4 TileEntries[];
        };

        // Per frame in flight, read back FramesInFlight frames later: [0] the pool generation that wrote it, [1] the
        // tiles whose pages ran out, [2] the most pages in use.
        Buffer(13) TileFeedback
        {
            uint TileFeedbackData[];
        };

        // Mirrors Graphic::System::GrassGeneratePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            uvec4 Mode; // x = kMode*, y = this mode's first entry in Tiles, z = its entries (Init: the tile slots),
                        // w = the pool's pages
            uvec4 Pool; // GRASS-VRAM-BUDGET-1: x = the free-page stack's room (the most pages the pool can grow to;
                        // the tile headers follow the stack), y = the first page this frame's growth adds, z = the
                        // pages it adds (Free pushes them on the stack), w = 0
        };

        // GrassRenderer.cpp kGrassTileMode*.
        const uint kModeInit     = 0u;
        const uint kModeFree     = 1u;
        const uint kModeCount    = 2u;
        const uint kModeAllocate = 3u;
        const uint kModeWrite    = 4u;
        // Mirror GrassRenderer.cpp kGrassTile*.
        const uint kTileCells       = 32u;
        const uint kTilePageRecords = kGrassTilePageRecords;
        const uint kTilePages       = kTileCells * kTileCells / kTilePageRecords;
        const uint kTilePageWord    = 16u;
        const uint kTilePoolWords   = kGrassTilePoolWords;
        // GRASS-TILE-2, a tile's clusters: kTileClusterCells^2 cells, one build thread group (LocalSize) each; per
        // cluster kTileClusterWords header words from kTileClusterWord: its records, its first record in the tile,
        // its root box (min xyz, max xyz, OrderedBits).
        const uint kTileClusterCells = 8u;
        const uint kTileClusterSide  = kTileCells / kTileClusterCells;
        const uint kTileClusters     = kTileClusterSide * kTileClusterSide;
        const uint kTileClusterWord  = 32u;
        const uint kTileClusterWords = 8u;
        const uint kTileHeaderWords  = kTileClusterWord + kTileClusters * kTileClusterWords;
        // The tallest a blade's clump makes it (the build's clumpHeight, r.Clump).
        const float kMaxClumpHeight = 1.3;

        // The build's cluster (Count: its count and box; Write: its records' fade draws, ranked).
        shared uint  s_ClusterScale;
        shared uint  s_ClusterCount;
        shared uint  s_ClusterBox[6];
        shared float s_Fade[64];

        uint Hash( uint x )
        {
            x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
            return x;
        }
        float Fraction( inout uint state )
        {
            state = Hash( state + 0x9e3779b9u );
            return float( state >> 8u ) * ( 1.0 / 16777216.0 );
        }

        // Smooth value noise in 0..1 at world XZ over @p size cm cells, two octaves.
        float Lattice( ivec2 c, uint seed )
        {
            return float( Hash( uint( c.x ) * 0x27d4eb2du ^ uint( c.y ) * 0x165667b1u ^ seed ) >> 8u ) *
                   ( 1.0 / 16777216.0 );
        }
        float ValueNoise( vec2 at, uint seed )
        {
            ivec2 c = ivec2( floor( at ) );
            vec2  f = at - floor( at );
            f       = f * f * ( 3.0 - 2.0 * f );
            return mix( mix( Lattice( c, seed ), Lattice( c + ivec2( 1, 0 ), seed ), f.x ),
                        mix( Lattice( c + ivec2( 0, 1 ), seed ), Lattice( c + ivec2( 1, 1 ), seed ), f.x ), f.y );
        }
        float PatchNoise( vec2 world, float size, uint seed )
        {
            return ( 2.0 * ValueNoise( world / size, seed ) + ValueNoise( world * 2.0 / size, seed ^ 0x5bd1e995u ) ) /
                   3.0;
        }

        // The rotation of an orthonormal basis (Shepperd's), and back.
        vec4 QuatFromBasis( mat3 m )
        {
            float trace = m[0][0] + m[1][1] + m[2][2];
            if ( trace > 0.0 )
            {
                float s = sqrt( trace + 1.0 ) * 2.0;
                return vec4( ( m[1][2] - m[2][1] ) / s, ( m[2][0] - m[0][2] ) / s, ( m[0][1] - m[1][0] ) / s, 0.25 * s );
            }
            if ( m[0][0] > m[1][1] && m[0][0] > m[2][2] )
            {
                float s = sqrt( 1.0 + m[0][0] - m[1][1] - m[2][2] ) * 2.0;
                return vec4( 0.25 * s, ( m[1][0] + m[0][1] ) / s, ( m[2][0] + m[0][2] ) / s, ( m[1][2] - m[2][1] ) / s );
            }
            if ( m[1][1] > m[2][2] )
            {
                float s = sqrt( 1.0 + m[1][1] - m[0][0] - m[2][2] ) * 2.0;
                return vec4( ( m[1][0] + m[0][1] ) / s, 0.25 * s, ( m[2][1] + m[1][2] ) / s, ( m[2][0] - m[0][2] ) / s );
            }
            float s = sqrt( 1.0 + m[2][2] - m[0][0] - m[1][1] ) * 2.0;
            return vec4( ( m[2][0] + m[0][2] ) / s, ( m[2][1] + m[1][2] ) / s, 0.25 * s, ( m[0][1] - m[1][0] ) / s );
        }

        // Draws in [0, 1) on the pool's 16-bit grid (rounded down: they stay below 1 and pack exactly).
        vec2 OnUnorm16( vec2 v )
        {
            return floor( v * 65535.0 ) / 65535.0;
        }

        void StoreRecord( uint at, TileRecord r )
        {
            GrassQuads quads = GrassQuads( TileSegmentData[at / kGrassTileSegmentRecords] );
            uint       quad  = GrassSegmentRecord( at ) * kTilePoolWords;
            quads.Quads[quad] = uvec4( floatBitsToUint( r.Position ), packUnorm2x16( vec2( r.Fade, r.Thin ) ) );
            quads.Quads[quad + 1u] = uvec4( floatBitsToUint( r.Scale ), floatBitsToUint( r.Clump ) );
            quads.Quads[quad + 2u] =
                 uvec4( packSnorm2x16( r.Rotation.xy ), packSnorm2x16( r.Rotation.zw ), packUnorm2x16( r.Draws.xy ),
                        packUnorm2x16( vec2( r.Draws.z, 0.0 ) ) );
        }

        // Floats as uints that order like them (the tile box's atomicMin / atomicMax), and back.
        uint OrderedBits( float f )
        {
            uint u = floatBitsToUint( f );
            return ( u & 0x80000000u ) != 0u ? ~u : u | 0x80000000u;
        }
        float FromOrderedBits( uint u )
        {
            return uintBitsToFloat( ( u & 0x80000000u ) != 0u ? u & 0x7fffffffu : ~u );
        }
        uint HeaderBase()
        {
            return 1u + Pool.x;
        }
        uint InitialHeaderWord( uint word )
        {
            return word >= 4u && word < 7u ? 0xffffffffu : 0u;
        }

        // THE BUILD (GRASS-TILE-1): the view-independent instance of @p cell, kept when its fade draw beats the
        // tile's share @p tileShare (GrassFieldShareAt at the nearest the tile is built for) - the generation's own
        // rules, moved.
        bool PlaceCell( ivec2 cell, GrassParams p, float tileShare, out TileRecord r )
        {
            r.Clump = 1.0;
            uint  state = Hash( uint( cell.x ) * 0x8da6b343u ^ uint( cell.y ) * 0xd8163841u ^ p.Info.w );
            // The lens share's draw (below): a hash of its own, so the share does not reshuffle the cell's jitter,
            // scale or rotation.
            uint  thin     = Hash( state ^ 0x5bd1e995u );
            float thinDraw = Fraction( thin );

            // UE's grid branch: the cell centre plus up to PlacementJitter half-cells.
            float cellSize = p.Cull.z;
            vec2  jitter   = vec2( Fraction( state ), Fraction( state ) ) * 2.0 - 1.0;
            vec2  world    = ( vec2( cell ) + 0.5 + jitter * p.Cull.w ) * cellSize;
            if ( p.Spread.x > 0.0 )
            {
                // Scatter: the blade strays past its own cell, so the field is no longer one stalk per cell.
                float angle = Fraction( state ) * 6.28318530718;
                float reach = sqrt( Fraction( state ) ) * p.Spread.x;
                world += vec2( cos( angle ), sin( angle ) ) * reach * cellSize;
            }

            uint  flags = floatBitsToUint( p.ScaleX.w );
            bool  blade = ( flags & kFlagBlade ) != 0u;
            uint  clumpSeed = 0u;
            if ( blade )
            {
                // The nearest clump point of the 3x3 clump cells around the blade; the blade moves towards it.
                vec2  clumpCell = floor( world / p.Blade.y );
                float best      = 3.0e38;
                vec2  clumpAt   = world;
                for ( int dz = -1; dz <= 1; ++dz )
                    for ( int dx = -1; dx <= 1; ++dx )
                    {
                        ivec2 c  = ivec2( clumpCell ) + ivec2( dx, dz );
                        uint  h  = Hash( uint( c.x ) * 0x27d4eb2du ^ uint( c.y ) * 0x165667b1u ^ ( p.Info.w * 0x9e3779b9u ) );
                        uint  hs = h;
                        vec2  at = ( vec2( c ) + vec2( Fraction( hs ), Fraction( hs ) ) ) * p.Blade.y;
                        float d  = dot( at - world, at - world );
                        if ( d < best )
                        {
                            best      = d;
                            clumpSeed = h;
                            clumpAt   = at;
                        }
                    }
                world = mix( world, clumpAt, p.Blade.w );
            }

            float height, weight;
            if ( !SampleField( world, p.Info.x, height, weight ) )
                return false;
            float keep = Fraction( state );
            // Patchiness: broad world patches, denser and sparser, thin up to that share of the blades.
            float patchy = weight;
            if ( p.Spread.y > 0.0 )
                patchy *= 1.0 - p.Spread.y * PatchNoise( world, p.Spread.z, p.Info.w );
            // Patches: the variety grows only where its own world noise rises above the share it leaves bare,
            // fading in over Softness at a patch's rim (UE: a noise term in the landscape grass output's weight).
            if ( p.Patches.x < 1.0 )
            {
                float rim = 1.0 - p.Patches.x;
                float rimBand = 0.5 * p.Patches.z;
                patchy *= smoothstep( rim - rimBand, rim + rimBand + 1.0e-4,
                                      PatchNoise( world, p.Patches.y, floatBitsToUint( p.Patches.w ) ) );
            }
            if ( !( weight > p.Range.x && weight <= p.Range.y && patchy >= keep ) )
                return false;


            // The draw the view's thinning drops the instance by (GrassCull.shader GrassViewLaw), on the pool's grid; the tile keeps
            // only the instances its nearest view can keep (the thinning is nested).
            float fade   = Fraction( state );
            vec2  drawn  = OnUnorm16( vec2( fade, thinDraw ) );
            r.Fade       = drawn.x;
            r.Thin       = drawn.y;
            if ( r.Fade >= tileShare )
                return false;
            // UE FGrassBuilderBase::GetRandomScale (Z is up, the engine's Y).
            float maxBias = ( flags & kFlagWeightAttenuates ) != 0u ? mix( 1.0 - p.Range.z, 1.0, weight ) : 1.0;
            float rx      = Fraction( state ) * maxBias;
            float ry      = Fraction( state ) * maxBias;
            float rz      = Fraction( state ) * maxBias;
            vec3  scale;
            if ( p.ScaleX.z < 0.5 )
                scale = vec3( mix( p.ScaleX.x, p.ScaleX.y, rx ) );
            else if ( p.ScaleX.z < 1.5 )
                scale = vec3( mix( p.ScaleX.x, p.ScaleX.y, rx ), mix( p.ScaleZ.x, p.ScaleZ.y, rz ),
                              mix( p.ScaleY.x, p.ScaleY.y, ry ) );
            else
                scale = vec3( mix( p.ScaleX.x, p.ScaleX.y, rx ), mix( p.ScaleZ.x, p.ScaleZ.y, rz ),
                              mix( p.ScaleX.x, p.ScaleX.y, rx ) );
            float yaw = ( flags & kFlagRandomRotation ) != 0u ? Fraction( state ) * 6.28318530718 : 0.0;
            vec3  draws = vec3( 0.0 );
            if ( blade )
            {
                float clumpYaw    = Fraction( clumpSeed ) * 6.28318530718;
                float clumpHeight = mix( 0.7, kMaxClumpHeight, Fraction( clumpSeed ) );
                float clumpTint   = Fraction( clumpSeed );
                // The facing turns towards the clump's by the shorter arc.
                float turn = mod( clumpYaw - yaw + 3.14159265359, 6.28318530718 ) - 3.14159265359;
                yaw += turn * p.Blade.z;
                // The blade's own facing back on top of its clump's (GoT: per-blade random facing).
                if ( p.Lean.y > 0.0 )
                    yaw += ( Fraction( state ) * 2.0 - 1.0 ) * p.Lean.y;
                r.Clump = mix( 1.0, clumpHeight, p.Blade.z );
                draws = vec3( Fraction( state ), Fraction( state ), mix( Fraction( state ), clumpTint, p.Blade.z ) );
            }
            float c = cos( yaw ), s = sin( yaw );
            mat3  basis = mat3( vec3( c, 0.0, -s ), vec3( 0.0, 1.0, 0.0 ), vec3( s, 0.0, c ) );
            if ( blade && p.Lean.x > 0.0 )
            {
                // Tilt (GoT): the whole blade turns off its up axis by up to Tilt, about a horizontal axis of its
                // own, so roots no longer all rise upright and the leans of neighbours differ in direction.
                float azimuth = Fraction( state ) * 6.28318530718;
                float angle   = Fraction( state ) * p.Lean.x;
                vec3  axis    = vec3( cos( azimuth ), 0.0, sin( azimuth ) );
                float ca = cos( angle ), sa = sin( angle );
                mat3  axisCross = mat3( vec3( 0.0, axis.z, -axis.y ), vec3( -axis.z, 0.0, axis.x ),
                                        vec3( axis.y, -axis.x, 0.0 ) );
                mat3  tilt = mat3( ca ) + sa * axisCross + ( 1.0 - ca ) * outerProduct( axis, axis );
                basis      = tilt * basis;
            }
            if ( ( flags & kFlagAlignToSurface ) != 0u )
            {
                // UE ApplyNormalAlignmentTransform: up becomes the ground normal.
                vec3 up    = FieldNormal( world, p.Info.x, height );
                vec3 right = normalize( cross( up, vec3( 0.0, 0.0, 1.0 ) ) );
                mat3 align = mat3( right, up, cross( right, up ) );
                basis      = align * basis;
            }
            r.Position = vec3( world.x, height, world.y );
            r.Scale    = scale;
            r.Rotation = QuatFromBasis( basis );
            r.Draws    = vec3( OnUnorm16( draws.xy ), OnUnorm16( vec2( draws.z, 0.0 ) ).x );
            return true;
        }

        // Record @p slot of the tile whose header is at @p header: its index in the pool.
        uint TileRecordAt( uint header, uint slot )
        {
            return TileWordsData[header + kTilePageWord + slot / kTilePageRecords] * kTilePageRecords +
                   slot % kTilePageRecords;
        }

        // GRASS-VIEW-LAW-1, THE BUILD'S CHUNK of cluster @p cluster (its header words) of the tile at @p header, chunk
        // @p chunk of Chunks (Common/GrassVarietyRows.glslh, the chunk contract): its records' root box, the largest
        // axis scale @p scale of its records (RecordScale bits), its first record in the pool, its count and the page
        // its run continues on. A run is one cluster's, at most one page long, so it spans two pages at most.
        void WriteTileChunk( uint header, uint cluster, uint chunk, uint scale )
        {
            uint first = TileWordsData[cluster + 1u];
            uint room  = TileWordsData[header + 1u] * kTilePageRecords;
            uint count = first < room ? min( TileWordsData[cluster], room - first ) : 0u;
            uint at    = GrassChunkAt( chunk );
            for ( uint i = 0u; i < 6u; ++i )
                GrassChunkData[at + kGrassChunkBoxWord + i] = floatBitsToUint( FromOrderedBits( TileWordsData[cluster + 2u + i] ) );
            GrassChunkData[at + kGrassChunkScaleWord] = scale;
            GrassChunkData[at + kGrassChunkFirstWord] = count != 0u ? TileRecordAt( header, first ) : 0u;
            uint next = first / kTilePageRecords + 1u;
            GrassChunkData[at + kGrassChunkPageWord] =
                 count != 0u && next < kTilePages ? TileWordsData[header + kTilePageWord + next] : 0u;
            GrassChunkData[at + kGrassChunkCountWord] = count;
            // The chunk's records are new: GrassCull laws them anew (the chunk contract's law words).
            GrassChunkData[at + kGrassChunkLawWord] = 0u;
        }

        // The largest axis scale record @p r can draw at before the view law widens it (GrassViewLaw: a blade's
        // height takes its clump's, a mesh only shrinks).
        float RecordScale( TileRecord r, GrassParams p )
        {
            float clump = ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) != 0u ? r.Clump : 1.0;
            return max( r.Scale.x, max( r.Scale.y * clump, r.Scale.z ) );
        }

        void main()
        {
            GrassParams p    = GrassParamsData[0];
            uint        serial = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
            if ( Mode.x == kModeInit )
            {
                // A new (or grown and reset) pool: every page free, every slot empty, every chunk empty - a reset pool
                // keeps its chunk buffer, whose counts would name records of pages handed out again.
                if ( serial == 0u )
                    TileWordsData[0] = Mode.w;
                if ( serial < Mode.w )
                    TileWordsData[1u + serial] = serial;
                if ( serial < Mode.z * kTileHeaderWords )
                    TileWordsData[HeaderBase() + serial] = InitialHeaderWord( serial % kTileHeaderWords );
                if ( serial < Mode.z * kTileClusters )
                    GrassChunkData[GrassChunkAt( serial ) + kGrassChunkCountWord] = 0u;
                return;
            }
            if ( Mode.x == kModeFree )
            {
                // GRASS-VRAM-BUDGET-1: the pages a grown pool's new segments add go on the stack, free; no tile is
                // built again and no record moves.
                if ( serial < Pool.z )
                    TileWordsData[1u + atomicAdd( TileWordsData[0], 1u )] = Pool.y + serial;
                // A tile left the radius or is rebuilt: its pages go back on the stack, its slot is emptied.
                if ( serial >= Mode.z )
                    return;
                uint header = HeaderBase() + TileEntries[Mode.y + serial].x * kTileHeaderWords;
                uint held   = TileWordsData[header + 1u];
                uint at     = atomicAdd( TileWordsData[0], held );
                for ( uint k = 0u; k < held; ++k )
                    TileWordsData[1u + at + k] = TileWordsData[header + kTilePageWord + k];
                // Its chunks are emptied too: the view law reads no record of a freed slot.
                for ( uint c = 0u; c < kTileClusters; ++c )
                    GrassChunkData[GrassChunkAt( TileEntries[Mode.y + serial].x * kTileClusters + c ) +
                                   kGrassChunkCountWord] = 0u;
                for ( uint w = 0u; w < kTilePageWord; ++w )
                    TileWordsData[header + w] = InitialHeaderWord( w );
                return;
            }
            if ( Mode.x == kModeAllocate )
            {
                // The pages the counted candidates need, popped off the stack; none left: the tile holds none and
                // TileFeedback tells the renderer to grow the pool.
                if ( serial >= Mode.z )
                    return;
                uint header = HeaderBase() + TileEntries[Mode.y + serial].x * kTileHeaderWords;
                uint count  = min( TileWordsData[header], kTilePages * kTilePageRecords );
                uint needed = ( count + kTilePageRecords - 1u ) / kTilePageRecords;
                TileFeedbackData[0] = TileEntries[0].x;
                // Each cluster's run of the tile's records, in cluster order.
                uint first = 0u;
                for ( uint c = 0u; c < kTileClusters; ++c )
                {
                    uint word                = header + kTileClusterWord + c * kTileClusterWords;
                    TileWordsData[word + 1u] = first;
                    first += TileWordsData[word];
                }
                if ( needed == 0u )
                    return;
                uint top = atomicAdd( TileWordsData[0], 0u - needed );
                if ( int( top ) < int( needed ) )
                {
                    atomicAdd( TileWordsData[0], needed );
                    atomicAdd( TileFeedbackData[1], 1u );
                    return;
                }
                for ( uint k = 0u; k < needed; ++k )
                    TileWordsData[header + kTilePageWord + k] = TileWordsData[1u + top - needed + k];
                TileWordsData[header + 1u] = needed;
                atomicMax( TileFeedbackData[2], Mode.w - ( top - needed ) );
                return;
            }
            if ( Mode.x == kModeCount || Mode.x == kModeWrite )
            {
                // One thread group per cluster (8 x 8 cells), kTileClusterSide squared per tile, one tile per z. No
                // return before the barriers: the mode and the entry are uniform over the group.
                uvec4 entry   = TileEntries[Mode.y + gl_WorkGroupID.z];
                uint  header  = HeaderBase() + entry.x * kTileHeaderWords;
                uint  cluster = HeaderBase() + entry.x * kTileHeaderWords + kTileClusterWord +
                               ( gl_WorkGroupID.y * kTileClusterSide + gl_WorkGroupID.x ) * kTileClusterWords;
                ivec2 cell    = ivec2( int( entry.y ), int( entry.z ) ) * int( kTileCells ) + ivec2( gl_GlobalInvocationID.xy );
                const GrassFieldShare built =
                     GrassFieldShareAt( uintBitsToFloat( entry.w ), p.Cull.x, p.Cull.y, p.Spread.w );
                TileRecord r;
                bool       placed = PlaceCell( cell, p, min( built.Kept, built.Floor ), r );
                uint       lane   = gl_LocalInvocationIndex;
                if ( Mode.x == kModeCount )
                {
                    // The cluster's count and root box in shared memory, then once into its header words and the
                    // tile's.
                    if ( lane == 0u )
                    {
                        s_ClusterCount = 0u;
                        for ( uint i = 0u; i < 3u; ++i )
                        {
                            s_ClusterBox[i]      = 0xffffffffu;
                            s_ClusterBox[3u + i] = 0u;
                        }
                    }
                    barrier();
                    if ( placed )
                    {
                        atomicAdd( s_ClusterCount, 1u );
                        for ( uint i = 0u; i < 3u; ++i )
                        {
                            atomicMin( s_ClusterBox[i], OrderedBits( r.Position[i] ) );
                            atomicMax( s_ClusterBox[3u + i], OrderedBits( r.Position[i] ) );
                        }
                    }
                    barrier();
                    if ( lane == 0u )
                    {
                        TileWordsData[cluster] = s_ClusterCount;
                        for ( uint i = 0u; i < 6u; ++i )
                            TileWordsData[cluster + 2u + i] = s_ClusterBox[i];
                        if ( s_ClusterCount != 0u )
                        {
                            atomicAdd( TileWordsData[header], s_ClusterCount );
                            for ( uint i = 0u; i < 3u; ++i )
                            {
                                atomicMin( TileWordsData[header + 4u + i], s_ClusterBox[i] );
                                atomicMax( TileWordsData[header + 7u + i], s_ClusterBox[3u + i] );
                            }
                        }
                    }
                    return;
                }
                // Write: the cluster's run, its records ranked by their fade draw (ties by lane), so the cull reads
                // only the head of the run the view's share keeps. A cell with no instance ranks past every one.
                // The chunk (WriteTileChunk) takes the largest axis scale of the cluster's records.
                if ( lane == 0u )
                    s_ClusterScale = 0u;
                barrier();
                s_Fade[lane] = placed ? r.Fade : 2.0;
                if ( placed )
                    atomicMax( s_ClusterScale, floatBitsToUint( RecordScale( r, p ) ) );
                barrier();
                if ( lane == 0u )
                    WriteTileChunk( header, cluster, entry.x * kTileClusters +
                                                         gl_WorkGroupID.y * kTileClusterSide + gl_WorkGroupID.x, s_ClusterScale );
                if ( !placed )
                    return;
                uint rank = 0u;
                for ( uint j = 0u; j < 64u; ++j )
                {
                    float other = s_Fade[j];
                    if ( other < r.Fade || ( other == r.Fade && j < lane ) )
                        ++rank;
                }
                uint slot = TileWordsData[cluster + 1u] + rank;
                if ( slot >= TileWordsData[header + 1u] * kTilePageRecords )
                    return;
                StoreRecord( TileRecordAt( header, slot ), r );
                return;
            }
        }
    }
}

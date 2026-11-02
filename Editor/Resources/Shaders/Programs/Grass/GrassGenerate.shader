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
// THE TILE CULL (Cull: one group per resident tile, each frame the view's inputs change - GrassRenderer
// VarietyGpu::GenerateKey) is everything view-dependent, the rules the generation applied before (GRASS-TILE-1 moved
// them unchanged). GRASS-TILE-2, TWO LEVELS (UE HISM: the cluster tree's bounds are culled first, then only the
// instances of the clusters that passed): a tile is built as kTileClusters clusters of kTileClusterCells^2 cells -
// one build thread group each - with their own root box and their records in a run of the tile's records SORTED BY
// THEIR FADE DRAW; the cull skips a tile, then a cluster, whose box lies past the cull radius or outside the full
// list's volume, and of a cluster it reads only the run's head whose fade draw beats the share at the box's nearest
// point (the thinning is nested and the share only falls with distance: the tail is dropped at any point of the
// box). So the cull's work follows the instances the view keeps, not the disc. An instance is kept when it is
// inside the view's side planes, and - past
// StartCullDistance - when its fade draw beats the distance share: (Start / d)^2, a blade's screen area
// (GRASS-DENSITY), times UE's fade to zero at EndCullDistance - and a kept blade widens by that share and lowers over
// the band's last fifth, a kept mesh shrinks to nothing before its draw drops it (GPU-COST-29), UE's (and past the
// distance D where its mean blade stands MinScreenWidth pixels wide, (D / d)^2 kept and each card lifted to that
// width - GRASS-WIDTH, no sub-pixel strip), PerInstanceFadeAmount as its grass materials spend it (scale to nothing
// at EndCullDistance), so the field thins and lowers into the ground instead of ending at a line. Kept instances are
// appended to their bucket of Instances (one mat4 each, the InstanceTransforms of Mesh/Surface/Vertex_Instanced.glslh
// / Vertex_GrassBlade.glslh): a mesh has one bucket per level of its LOD chain, chosen by the instance's screen size
// (UE grass HISM), a blade one per level of its strip, chosen by distance (GRASS-BLADE). A blade also follows its
// clump - the nearest of a jittered grid of clump points (GoT's Voronoi clumps) lends it its facing, height and tint,
// and pulls it towards itself by ClumpPull, so blades gather into tufts with open ground between them. Scatter moves
// a blade past its own cell and Patchiness thins the field by broad world patches. FacingJitter turns each blade's
// facing back off its clump's and Tilt stands its root off the vertical in a direction of its own. GrassFinalize
// copies each bucket's count into its draw records.
// OCCLUSION PER VIEW (GRASS-VIS; UE Nanite / the GPU instance cull): Instances / Args hold every kept instance: the
// cascades and the TLAS draw them, so grass the camera cannot see still casts and reflects. The camera's own list
// (CameraIds / CameraArgs, the same bucket table) is GrassCull.shader's pass over those kept instances
// (GRASS-CULL-1/2: one thread per KEPT instance, indirect; two-phase HZB). THE LISTS HOLD IDS (GPU-COST-22; UE GPU
// Scene InstanceIdBuffer): an instance is written ONCE, into its row of Instances; the camera's list and every
// cascade's hold that row (one uint each), and the vertex stage reads Instances through the id. THE TWO VOLUMES
// (GRASS-VIS-6; UE: the casters of a whole-scene shadow are culled by the cascade's volume swept along the light):
// Planes bound the full list - the camera's side planes, each moved out along every shadowed directional light by
// that light's caster reach - and CameraPlanes the camera's own list (GrassCull's).
Shader "GrassGenerate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GrassFieldShare.glslh>
        #include <Common/ShadowWind.glslh>

        // Mirrors Graphic::System::GrassParamsGpu (GrassRenderer.hpp), std430.
        struct GrassParams
        {
            vec4  Planes[4];       // the full list's: the camera's side planes swept along the shadowed lights, xyz.n + w >= 0 inside
            vec4  CameraPlanes[4]; // the camera's left / right / bottom / top planes (its own list)
            vec4  Camera;    // xyz = view position (cm), w = EndCullDistance through the lens
            vec4  Cull;      // x, y = Start/EndCullDistance through the lens (DrawDistanceScale), z = cell (cm), w = max jitter (cells)
            vec4  Range;     // x/y = AllowedDensityRange, z = MaxScaleWeightAttenuation, w = bounds radius (local)
            vec4  ScaleX;    // x/y = ScaleX range, z = scaling (0 uniform, 1 free, 2 lock XY), w = flags
            vec4  ScaleY;    // x/y = ScaleY range, z = bounds centre height (local)
            vec4  ScaleZ;    // x/y = ScaleZ range (up), w = the floor of the lens share (1 / DrawDistanceScale, at most 1; LensShare)
            uvec4 Info;      // x = layer, y = capacity, z = submesh count, w = seed
            uvec4 Buckets;   // x = bucket count, y = the bucket table's first word in Args, z = a blade's card count
            vec4  Blade;     // x = LodDistance through the lens (0: one bucket), y = ClumpSize cm, z = Clumping, w = ClumpPull
            vec4  Spread;    // x = Scatter (cells a blade may stray past its own), y = Patchiness, z = PatchSize cm,
                             // w = the screen-width floor's distance D (cm; 0: none), the mean blade MinScreenWidth px
            vec4  Lean;      // x = Tilt (radians), y = FacingJitter (radians), z = the world's SnowDepth (cm),
                             // w = the screen-width floor's width per unit of distance (MinScreenWidth px; 0: none)
            vec4  Lod;       // a mesh's levels by screen size: x = the lens's screen multiple x local half-diagonal, y = |Projection[2][3]|;
                             // the far LOD of a blade or a mesh: z = the distance at which its bounding radius at scale 1
                             // stands at the Foliage quality's LodScreenPixels (0: no far LOD; GrassFieldShare.glslh
                             // GrassBladeLodAt, GrassMeshFarLod)
            vec4  Patches;   // the variety's world patches: x = Coverage (1: none), y = Size cm, z = Softness, w = seed bits
            vec4  CardWidths[2]; // a blade's cards' widths, cm at scale 1: card i at [i / 4][i % 4], Buckets.z cards
            uvec4 Cascades;  // the cascades' lists (GPU-COST-21): x = lists (0: none), y = words of one list in
                             // CascadeArgs (its copy of Args' layout, then its rest twin), z = the cull pad (float bits,
                             // cm: a mesh's sway past its bounds), w = a list's first rest count (one per bucket)
            vec4  CascadePlanes[48]; // list k's caster box at 6k..6k+5 (GrassRenderer kGrassCascadeLists = 8 lists)
            vec4  CasterWindOrigin;  // GRASS-SHADOW-1: xyz = the shadow wind rule's origin (the main camera)
            vec4  CascadeRings[8];   // list k's movers' ring: x = inner, y = wind distance (0: no mover), cm
        };

        ReadBuffer(0) Field
        {
            uint FieldData[]; // GrassFieldLayout (LandscapeGrassField.hpp)
        };

        ReadBuffer(1) Params
        {
            GrassParams GrassParamsData[];
        };

        Buffer(2) Instances
        {
            mat4 GrassInstancesData[];
        };

        Buffer(3) Args
        {
            uint Arguments[]; // GpuInstances.hpp: header, the draw records, then per bucket count, first, capacity, half screen size (a mesh level)
        };

        // THE CASCADES' LISTS (GPU-COST-21; UE: every shadow view culls its casters by its own volume): per live
        // cascade view a list of the instances inside that cascade's caster box, the same records and bucket table
        // as Args, list k's copy at k x Cascades.y words, its buckets at k x capacity in CascadeIds (per slot the
        // instance's row of Instances, GPU-COST-22). The
        // cascade draws its list instead of the full one (MeshRenderer::BuildGpuInstancedShadowDraws).
        // EACH LAYER ITS OWN PART (GRASS-SHADOW-1; UE: a cached shadow's static and movable casters are separate
        // draw lists): a bucket's movers - the instances in the list's ring CascadeRings[k] of the rule's origin,
        // the ones that sway over the cascade's cache - fill it from its first slot, counted by the bucket table;
        // its rest-pose casters fill it from its last slot, counted by the list's rest counts (Cascades.w). The
        // cache's static layer draws the rest part, the movable layer the movers: neither runs a vertex of the
        // other's instances. A bucket's two parts never meet: together they hold at most what the full list kept.
        Buffer(8) CascadeIds
        {
            uint GrassCascadeIdsData[];
        };

        Buffer(9) CascadeArgs
        {
            uint CascadeArguments[];
        };

        // GRASS-TILE-1, THE TILE POOL: kTilePoolWords uvec4 per record (TileRecord: StoreRecord / LoadRecord), in
        // pages of kTilePageRecords records; a tile holds up to kTilePages of them (its TileWords header).
        Buffer(10) TilePool
        {
            uvec4 TilePoolData[];
        };

        // The pool's bookkeeping: [0] the free pages' count, [1, 1 + pages) the free-page stack, then per tile slot
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
        };

        const uint kFlagRandomRotation = 1u;
        const uint kFlagAlignToSurface = 2u;
        const uint kFlagWeightAttenuates = 4u;
        const uint kFlagBlade = 8u;
        const float kNoHeight = -1.0e30;
        // GrassRenderer.cpp kGrassTileMode*.
        const uint kModeInit     = 0u;
        const uint kModeFree     = 1u;
        const uint kModeCount    = 2u;
        const uint kModeAllocate = 3u;
        const uint kModeWrite    = 4u;
        const uint kModeCull     = 5u;
        // Mirror GrassRenderer.cpp kGrassTile*.
        const uint kTileCells       = 32u;
        const uint kTilePageRecords = 64u;
        const uint kTilePages       = kTileCells * kTileCells / kTilePageRecords;
        const uint kTilePageWord    = 16u;
        const uint kTilePoolWords   = 3u;
        // GRASS-TILE-2, a tile's clusters: kTileClusterCells^2 cells, one build thread group (LocalSize) each; per
        // cluster kTileClusterWords header words from kTileClusterWord: its records, its first record in the tile,
        // its root box (min xyz, max xyz, OrderedBits).
        const uint kTileClusterCells = 8u;
        const uint kTileClusterSide  = kTileCells / kTileClusterCells;
        const uint kTileClusters     = kTileClusterSide * kTileClusterSide;
        const uint kTileClusterWord  = 32u;
        const uint kTileClusterWords = 8u;
        const uint kTileHeaderWords  = kTileClusterWord + kTileClusters * kTileClusterWords;
        // The most the far LOD widens a blade's sphere (CullCandidate lod.Widen: 1 / GrassFarLodShare, at most 2,
        // times GrassBladeFadeWiden, at most 2).
        const float kMaxLodWiden = 4.0;

        // The build's cluster (Count: its count and box; Write: its records' fade draws, ranked) and the cull's
        // clusters (their kept runs).
        shared uint  s_ClusterCount;
        shared uint  s_ClusterBox[6];
        shared float s_Fade[64];
        shared uint  s_Keep[kTileClusters];
        shared uint  s_First[kTileClusters];

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

        uint SamplesX() { return FieldData[0]; }
        uint SamplesZ() { return FieldData[1]; }
        float HeightAt( uint x, uint z ) { return uintBitsToFloat( FieldData[8u + z * SamplesX() + x] ); }
        float WeightAt( uint layer, uint x, uint z )
        {
            uint count = SamplesX() * SamplesZ();
            uint index = z * SamplesX() + x;
            uint word  = FieldData[8u + count + layer * ( ( count + 3u ) / 4u ) + index / 4u];
            return float( ( word >> ( 8u * ( index & 3u ) ) ) & 0xffu ) * ( 1.0 / 255.0 );
        }

        // Bilinear height and layer weight at world XZ; false outside the field or over a missing tile.
        bool SampleField( vec2 world, uint layer, out float height, out float weight )
        {
            vec2  origin  = vec2( uintBitsToFloat( FieldData[4] ), uintBitsToFloat( FieldData[5] ) );
            float spacing = uintBitsToFloat( FieldData[6] );
            vec2  local   = ( world - origin ) / spacing;
            if ( local.x < 0.0 || local.y < 0.0 || local.x >= float( SamplesX() - 1u ) || local.y >= float( SamplesZ() - 1u ) )
                return false;
            uvec2 i = uvec2( floor( local ) );
            vec2  f = local - vec2( i );
            float h00 = HeightAt( i.x, i.y ), h10 = HeightAt( i.x + 1u, i.y );
            float h01 = HeightAt( i.x, i.y + 1u ), h11 = HeightAt( i.x + 1u, i.y + 1u );
            if ( min( min( h00, h10 ), min( h01, h11 ) ) <= kNoHeight )
                return false;
            height = mix( mix( h00, h10, f.x ), mix( h01, h11, f.x ), f.y );
            weight = mix( mix( WeightAt( layer, i.x, i.y ), WeightAt( layer, i.x + 1u, i.y ), f.x ),
                          mix( WeightAt( layer, i.x, i.y + 1u ), WeightAt( layer, i.x + 1u, i.y + 1u ), f.x ), f.y );
            return true;
        }

        vec3 FieldNormal( vec2 world, uint layer, float centre )
        {
            float spacing = uintBitsToFloat( FieldData[6] );
            float hx, hz, w;
            if ( !SampleField( world + vec2( spacing, 0.0 ), layer, hx, w ) )
                hx = centre;
            if ( !SampleField( world + vec2( 0.0, spacing ), layer, hz, w ) )
                hz = centre;
            return normalize( vec3( centre - hx, spacing, centre - hz ) );
        }

        // The share of cells a zoomed lens (DrawDistanceScale k > 1) keeps at @p world: the one that puts as many
        // instances on a pixel as the authored 90-degree lens puts there when it views the same ground patch from
        // the same height at 1/k the horizontal distance (where the patch fills the same screen size). A ground
        // patch of area A at distance d, seen at cosine s = dot(V, N) between the eye direction and the TERRAIN
        // normal, covers A s / d^2 of the view through a lens that magnifies k^2 times; equal instances per pixel
        // then means share = k^2 (s / s') (d' / d)^2 for the authored view's s', d'. Flat ground far away gives
        // 1/k (the floor, p.ScaleZ.w); a slope facing the camera - or ground under it - gives the full density,
        // which a flat 1/k share thinned visibly. Ground facing away keeps the floor.
        float LensShare( vec2 world, float height, GrassParams p )
        {
            float k     = 1.0 / p.ScaleZ.w;
            vec3  toEye = p.Camera.xyz - vec3( world.x, height, world.y );
            float rise  = toEye.y;
            vec2  across = toEye.xz;
            float d2    = dot( toEye, toEye );
            vec3  n     = FieldNormal( world, p.Info.x, height );
            float s     = dot( toEye, n ) * inversesqrt( max( d2, 1.0 ) );
            if ( s <= 0.0 )
                return p.ScaleZ.w;
            vec3  authored  = vec3( across.x / k, rise, across.y / k );
            float authored2 = dot( authored, authored );
            float s2        = dot( authored, n ) * inversesqrt( max( authored2, 1.0 ) );
            if ( s2 <= s / k )
                return 1.0;
            return clamp( k * k * ( s / s2 ) * ( authored2 / max( d2, 1.0 ) ), p.ScaleZ.w, 1.0 );
        }

        // GRASS-TILE-1: what the tile pool holds of an instance - everything the view does not change: its root
        // (before the snow), its fade and lens-share draws, its scale before the view's thinning, a blade's clump
        // height, its rotation (yaw, tilt, surface alignment) and a blade's three draws.
        struct TileRecord
        {
            vec3  Position;
            float Fade;
            float Thin;
            vec3  Scale;
            float Clump;
            vec4  Rotation;
            vec3  Draws;
        };

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
        mat3 BasisFromQuat( vec4 q )
        {
            q       = normalize( q );
            float x = q.x, y = q.y, z = q.z, w = q.w;
            return mat3( vec3( 1.0 - 2.0 * ( y * y + z * z ), 2.0 * ( x * y + w * z ), 2.0 * ( x * z - w * y ) ),
                         vec3( 2.0 * ( x * y - w * z ), 1.0 - 2.0 * ( x * x + z * z ), 2.0 * ( y * z + w * x ) ),
                         vec3( 2.0 * ( x * z + w * y ), 2.0 * ( y * z - w * x ), 1.0 - 2.0 * ( x * x + y * y ) ) );
        }

        // Draws in [0, 1) on the pool's 16-bit grid (rounded down: they stay below 1 and pack exactly).
        vec2 OnUnorm16( vec2 v )
        {
            return floor( v * 65535.0 ) / 65535.0;
        }

        void StoreRecord( uint at, TileRecord r )
        {
            TilePoolData[at * kTilePoolWords] =
                 uvec4( floatBitsToUint( r.Position ), packUnorm2x16( vec2( r.Fade, r.Thin ) ) );
            TilePoolData[at * kTilePoolWords + 1u] = uvec4( floatBitsToUint( r.Scale ), floatBitsToUint( r.Clump ) );
            TilePoolData[at * kTilePoolWords + 2u] =
                 uvec4( packSnorm2x16( r.Rotation.xy ), packSnorm2x16( r.Rotation.zw ), packUnorm2x16( r.Draws.xy ),
                        packUnorm2x16( vec2( r.Draws.z, 0.0 ) ) );
        }
        TileRecord LoadRecord( uint at )
        {
            uvec4      a = TilePoolData[at * kTilePoolWords];
            uvec4      b = TilePoolData[at * kTilePoolWords + 1u];
            uvec4      c = TilePoolData[at * kTilePoolWords + 2u];
            TileRecord r;
            r.Position = uintBitsToFloat( a.xyz );
            vec2 drawn = unpackUnorm2x16( a.w );
            r.Fade     = drawn.x;
            r.Thin     = drawn.y;
            r.Scale    = uintBitsToFloat( b.xyz );
            r.Clump    = uintBitsToFloat( b.w );
            r.Rotation = vec4( unpackSnorm2x16( c.x ), unpackSnorm2x16( c.y ) );
            r.Draws    = vec3( unpackUnorm2x16( c.z ), unpackUnorm2x16( c.w ).x );
            return r;
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
            return 1u + Mode.w;
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


            // The draw the view's thinning drops the instance by (CullCandidate), on the pool's grid; the tile keeps
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
                float clumpHeight = mix( 0.7, 1.3, Fraction( clumpSeed ) );
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

        // The widest a candidate's bounding sphere (CullCandidate) can grow at most @p farthest from the eye, plus
        // its centre's height: the tile and cluster cull's pad. A blade widens by the thinning's Widen - it only
        // grows with distance, so the farthest point's bounds the box's - and by the far LOD's; a mesh only shrinks.
        float TilePad( GrassParams p, float farthest )
        {
            bool  blade = ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) != 0u;
            float s = max( max( abs( p.ScaleX.x ), abs( p.ScaleX.y ) ),
                           max( max( abs( p.ScaleY.x ), abs( p.ScaleY.y ) ), max( abs( p.ScaleZ.x ), abs( p.ScaleZ.y ) ) ) );
            float floorBound = 0.0;
            if ( p.Lean.w > 0.0 )
            {
                float narrowest = 1.0e30;
                for ( uint i = 0u; i < min( p.Buckets.z, 8u ); ++i )
                    narrowest = min( narrowest, p.CardWidths[i / 4u][i % 4u] );
                floorBound = p.Lean.w * farthest / max( narrowest, 1.0e-3 );
            }
            float widen    = blade ? GrassFieldShareAt( farthest, p.Cull.x, p.Cull.y, p.Spread.w ).Widen : 1.0;
            float lodWiden = blade ? kMaxLodWiden : 1.0;
            return p.Range.w * max( max( s * widen, floorBound ) * lodWiden, s ) + abs( p.ScaleY.z ) * s;
        }

        // A root box (tile or cluster) none of whose candidates the cull can keep: past the cull radius, or
        // outside the full list's volume by more than the pad.
        bool BoxCulled( GrassParams p, vec3 lo, vec3 hi )
        {
            if ( length( clamp( p.Camera.xyz, lo, hi ) - p.Camera.xyz ) > p.Cull.y )
                return true;
            float farthest = length( max( abs( lo - p.Camera.xyz ), abs( hi - p.Camera.xyz ) ) );
            float pad      = TilePad( p, farthest );
            vec3  middle   = 0.5 * ( lo + hi );
            vec3  extent   = 0.5 * ( hi - lo );
            for ( int i = 0; i < 4; ++i )
                if ( dot( p.Planes[i].xyz, middle ) + dot( abs( p.Planes[i].xyz ), extent ) + p.Planes[i].w < -pad )
                    return true;
            return false;
        }

        vec3 BoxWords( uint at )
        {
            return vec3( FromOrderedBits( TileWordsData[at] ), FromOrderedBits( TileWordsData[at + 1u] ),
                         FromOrderedBits( TileWordsData[at + 2u] ) );
        }

        // Record @p slot of the tile whose header is at @p header: its index in the pool.
        uint TileRecordAt( uint header, uint slot )
        {
            return TileWordsData[header + kTilePageWord + slot / kTilePageRecords] * kTilePageRecords +
                   slot % kTilePageRecords;
        }

        // THE TILE CULL of one candidate (GRASS-TILE-1): everything the view decides - the generation's own rules,
        // moved from the build.
        void CullCandidate( TileRecord r, GrassParams p )
        {
            vec2  world  = r.Position.xz;
            float height = r.Position.y;
            if ( r.Thin >= p.ScaleZ.w && r.Thin >= LensShare( world, height, p ) )
                return;
            uint flags = floatBitsToUint( p.ScaleX.w );
            bool blade = ( flags & kFlagBlade ) != 0u;
            vec3  position = r.Position;
            float distance = length( position - p.Camera.xyz );
            if ( distance > p.Cull.y )
                return;
            // The share of the field kept at this distance (GRASS-DENSITY, Common/GrassFieldShare.glslh - the one
            // law the ground's far cover reads too): beyond StartCullDistance S it falls with the square of the
            // distance, (S / d)^2, so the instances a band of ground spends stay those it spent at S (UE's grass
            // HISM keeps the full density out to its cull distance and spends it on sub-pixel instances) - and on
            // top of that UE's fade, down to nothing at EndCullDistance. Nested: an instance kept at d is kept
            // nearer, so walking does not reshuffle the field, only thins it; before S every instance is kept.
            float                 fade  = r.Fade;
            const GrassFieldShare field = GrassFieldShareAt( distance, p.Cull.x, p.Cull.y, p.Spread.w );
            float                 kept  = field.Kept;
            // The screen-width floor (GRASS-WIDTH; GoT / UE landscape grass LOD): past D the mean blade would stand
            // narrower than MinScreenWidth pixels, so the field keeps (D / d)^2 of its blades - the law the thinning
            // above widens by (a survivor widened 1/sqrt(share): d / D here) - and each kept blade's own card is
            // lifted to the floor width (below), so no blade strip goes sub-pixel. The same nested draw: a blade
            // kept at d is kept nearer.
            if ( fade >= min( kept, field.Floor ) )
                return;
            vec3 scale = r.Scale;
            // Coverage kept through the thinning (GoT / UE-style distance thinning; GrassFieldShare): a BLADE
            // survivor stands for the 1/share blades dropped around it, so its width grows by 1/sqrt(share) - at its
            // own height the field keeps sqrt(share) of its covered area, and the ground's far cover takes on the
            // rest (LandscapeGrassCover, GrassFieldCoverage); the height drops only over the last 20 % of the range.
            // A MESH survivor's cards cost their pixels, so it keeps its size and shrinks uniformly to nothing over
            // the last stretch before its own draw drops it - at EndCullDistance too (GPU-COST-29; UE grass HISM:
            // no survivor growth, PerInstanceFadeAmount / scale to zero; GrassMeshShrink, GrassMeshCoverage): its
            // card overdraw falls with distance as the field thins, and the cover takes on what it leaves.
            // THE BLADE'S LOD (GPU-FRAME-4, GrassFieldShare.glslh GrassBladeLodAt - the one choice): its strip level
            // and the far LOD's share from its bounding radius on screen - its own height scale, before the
            // thinning's height and its clump's, over the far LOD's screen size (p.Lod.z: where a blade at scale 1
            // stands at it). The far LOD drops the blades of the same nested draw the thinning above drops by (an
            // instance kept at a share is kept at every larger one), so the field thins one blade at a time and
            // never reshuffles; its survivors widen by 1 / Share below, past the floor's lift.
            GrassBladeLod lod = GrassBladeLod( 0u, 1.0, 1.0 );
            if ( blade )
            {
                uint  ring = p.Blade.x > 0.0 ? min( uint( distance / p.Blade.x ), p.Buckets.x - 1u ) : 0u;
                float size = p.Lod.z > 0.0 ? p.Lod.z * scale.y / max( distance, 1.0 ) : 0.0;
                lod        = GrassBladeLodAt( size, ring, p.Buckets.x );
                float share = min( kept, field.Floor ) * lod.Share;
                if ( fade >= share )
                    return;
                scale.x *= field.Widen;
                scale.z *= field.Widen;
                // The blade the thinning is about to drop lowers into the ground first (GrassBladeFade: stable per
                // instance, no pop), and the survivors widen by what that lowering takes from the field's area.
                scale.y *= field.Height * GrassBladeFade( share, fade );
                lod.Widen *= GrassBladeFadeWiden( share );
            }
            else
            {
                // THE MESH'S FAR LOD (GPU-FRAME-6, GrassFieldShare.glslh GrassMeshFarLod): the blade's far-LOD
                // share by its bounding radius on screen - its largest axis scale over the far LOD's screen size
                // (p.Lod.z: where the mesh at scale 1 stands at it) - on top of the distance thinning, and the
                // instance it drops shrinks to nothing first (GrassMeshShrink over both shares).
                float           size    = p.Lod.z > 0.0
                                              ? p.Lod.z * max( scale.x, max( scale.y, scale.z ) ) / max( distance, 1.0 )
                                              : 0.0;
                GrassFieldShare reached = GrassMeshFarLod( field, size );
                if ( fade >= reached.Kept )
                    return;
                scale *= GrassMeshShrink( reached, fade );
            }
            // The floor's width scale for this instance's card (below, once the card is drawn) is bounded by its
            // narrowest card's: the sphere tests take the widest the instance can become.
            float floorBound = 0.0;
            if ( p.Lean.w > 0.0 )
            {
                float narrowest = 1.0e30;
                for ( uint i = 0u; i < min( p.Buckets.z, 8u ); ++i )
                    narrowest = min( narrowest, p.CardWidths[i / 4u][i % 4u] );
                floorBound = p.Lean.w * distance / max( narrowest, 1.0e-3 );
            }

            // Inside the full list's volume (the camera's side planes swept along the lights), by the instance's
            // bounding sphere (the camera's own list is GrassCull.shader's).
            float radius = p.Range.w * max( max( scale.x, floorBound ) * lod.Widen, max( scale.y, scale.z ) );
            vec3  centre = position + vec3( 0.0, p.ScaleY.z * scale.y, 0.0 );
            for ( int i = 0; i < 4; ++i )
                if ( dot( p.Planes[i].xyz, centre ) + p.Planes[i].w < -radius )
                    return;
            vec3 draws = r.Draws;
            if ( blade )
            {
                scale.y *= r.Clump;
                // The screen-width floor (GRASS-WIDTH): the card this instance draws (Mesh/Surface/GrassBlade.glslh
                // BladeCard, from draws.x) stands at least MinScreenWidth pixels wide - its width scale lifted to
                // Lean.w x d / its width - so no strip of the blade goes sub-pixel.
                if ( p.Lean.w > 0.0 && p.Buckets.z > 0u )
                {
                    uint  count = min( p.Buckets.z, 8u );
                    uint  card  = min( uint( draws.x * float( count ) ), count - 1u );
                    float width = p.CardWidths[card / 4u][card % 4u];
                    scale.x     = max( scale.x, p.Lean.w * distance / max( width, 1.0e-3 ) );
                }
                // The far LOD's survivor stands for the 1 / Share blades it keeps the place of: its card - lifted to
                // the floor first - is that much wider, so the field covers what it did.
                scale.x *= lod.Widen;
            }
            mat3 basis = BasisFromQuat( r.Rotation );

            uint bucket = 0u;
            if ( blade )
                bucket = lod.Level;
            else if ( p.Buckets.x > 1u )
            {
                // A mesh's level (UE ComputeStaticMeshLOD over the grass HISM's instances, Geometry::
                // SelectLODFromBounds): the coarsest whose screen size the instance's bounding sphere is still below.
                float r        = p.Lod.x * max( scale.x, max( scale.y, scale.z ) );
                vec3  d        = centre - p.Camera.xyz;
                float screenSq = r * r / max( 1.0, dot( d, d ) * p.Lod.y );
                for ( uint level = p.Buckets.x - 1u; level > 0u; --level )
                {
                    float halfSize = uintBitsToFloat( Arguments[p.Buckets.y + level * 4u + 3u] );
                    if ( halfSize * halfSize > screenSq )
                    {
                        bucket = level;
                        break;
                    }
                }
            }
            uint table = p.Buckets.y + bucket * 4u;
            uint index = atomicAdd( Arguments[table], 1u );
            if ( index >= Arguments[table + 2u] )
                return;
            index += Arguments[table + 1u];
            // The world's snow (Common/WeatherSnow.glslh) lies p.Lean.z deep over the ground: the root sinks by it,
            // so the instance shows only what stands above the snow and one shorter than it is buried whole.
            position.y -= p.Lean.z;
            // A blade's forward axis carries the height multiplier (its lean is a share of its height) and the
            // columns' w its three draws (Vertex_GrassBlade.glslh); a mesh's are its scaled axes.
            mat4 instance;
            if ( blade )
                instance = mat4( vec4( basis[0] * scale.x, draws.x ), vec4( basis[1] * scale.y, draws.y ),
                                 vec4( basis[2] * scale.y, draws.z ), vec4( position, 1.0 ) );
            else
                instance = mat4( vec4( basis[0] * scale.x, 0.0 ), vec4( basis[1] * scale.y, 0.0 ),
                                 vec4( basis[2] * scale.z, 0.0 ), vec4( position, 1.0 ) );
            GrassInstancesData[index] = instance;

            // Into each cascade's list whose caster box (the six planes of the matrix the cascade draws with,
            // MeshRenderer::CascadeCasterBoxSet: its ortho box pulled back towards the light, so a caster between
            // the light and the slice is inside) the instance's reach meets: a sphere about the root - after the
            // snow sank it - holding its bounds at the final scale, the clump's height included, plus a mesh's sway.
            // A list's bucket gets at most what the full list's kept, so it never passes the capacity they share.
            if ( p.Cascades.x != 0u )
            {
                float reach = ( abs( p.ScaleY.z ) + p.Range.w ) * max( scale.x, max( scale.y, scale.z ) ) +
                              uintBitsToFloat( p.Cascades.z );
                for ( uint k = 0u; k < p.Cascades.x; ++k )
                {
                    bool inside = true;
                    for ( uint i = 0u; i < 6u && inside; ++i )
                    {
                        vec4 plane = p.CascadePlanes[k * 6u + i];
                        inside     = dot( plane.xyz, position ) + plane.w >= -reach;
                    }
                    if ( !inside )
                        continue;
                    // MeshRenderer::BuildGpuInstancedShadowDraws' rule on the instance's point (its root, the
                    // vertex stage's model[3]): a mover inside the ring [inner, wind distance), else its rest pose.
                    // The ring is Common/ShadowWind.glslh's one rule (ShadowWindInsideRing, as GpuCull_CastsRestPose
                    // and the CPU walk read it); x = ShadowWindInnerOf = ShadowWindRingInner( ShadowWindRingOf ).
                    vec2 ring  = p.CascadeRings[k].xy;
                    bool mover = ring.y > 0.0 &&
                                 !ShadowWindCastsRestPose( position, p.CasterWindOrigin.xyz, ring.y ) &&
                                 !ShadowWindInsideRing( position, p.CasterWindOrigin.xyz, ring.x );
                    uint at   = k * p.Cascades.y + table;
                    uint room = CascadeArguments[at + 2u];
                    if ( mover )
                    {
                        uint slot = atomicAdd( CascadeArguments[at], 1u );
                        if ( slot < room )
                            GrassCascadeIdsData[CascadeArguments[at + 1u] + slot] = index;
                    }
                    else
                    {
                        uint slot = atomicAdd( CascadeArguments[k * p.Cascades.y + p.Cascades.w + bucket], 1u );
                        if ( slot < room )
                            GrassCascadeIdsData[CascadeArguments[at + 1u] + room - 1u - slot] = index;
                    }
                }
            }
        }

        void main()
        {
            // The pass's own copy of the params (the camera cull, GrassCull.shader, makes none).
            GrassParams p    = GrassParamsData[0];
            uint        serial = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
            if ( Mode.x == kModeInit )
            {
                // A new pool: every page free, every slot empty.
                if ( serial == 0u )
                    TileWordsData[0] = Mode.w;
                if ( serial < Mode.w )
                    TileWordsData[1u + serial] = serial;
                if ( serial < Mode.z * kTileHeaderWords )
                    TileWordsData[HeaderBase() + serial] = InitialHeaderWord( serial % kTileHeaderWords );
                return;
            }
            if ( Mode.x == kModeFree )
            {
                // A tile left the radius or is rebuilt: its pages go back on the stack, its slot is emptied.
                if ( serial >= Mode.z )
                    return;
                uint header = HeaderBase() + TileEntries[Mode.y + serial].x * kTileHeaderWords;
                uint held   = TileWordsData[header + 1u];
                uint at     = atomicAdd( TileWordsData[0], held );
                for ( uint k = 0u; k < held; ++k )
                    TileWordsData[1u + at + k] = TileWordsData[header + kTilePageWord + k];
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
                s_Fade[lane] = placed ? r.Fade : 2.0;
                barrier();
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
            if ( Mode.x == kModeCull )
            {
                // One group per resident tile: the tile whole by its candidates' root box (uniform over the group),
                // then each cluster by its own (a thread each), then the clusters' kept runs, 64 records a round.
                uint header  = HeaderBase() + TileEntries[Mode.y + gl_WorkGroupID.x].x * kTileHeaderWords;
                uint records = min( TileWordsData[header], TileWordsData[header + 1u] * kTilePageRecords );
                if ( records == 0u || BoxCulled( p, BoxWords( header + 4u ), BoxWords( header + 7u ) ) )
                    return;
                uint lane = gl_LocalInvocationIndex;
                if ( lane < kTileClusters )
                {
                    uint word  = header + kTileClusterWord + lane * kTileClusterWords;
                    uint first = TileWordsData[word + 1u];
                    uint keep  = 0u;
                    if ( first < records && TileWordsData[word] != 0u )
                    {
                        uint count = min( TileWordsData[word], records - first );
                        vec3 lo    = BoxWords( word + 2u );
                        vec3 hi    = BoxWords( word + 5u );
                        if ( !BoxCulled( p, lo, hi ) )
                        {
                            // The run is sorted by fade draw: its records from the first one the share at the box's
                            // nearest point drops are dropped at every point of the box (CullCandidate's thinning).
                            float nearest = length( clamp( p.Camera.xyz, lo, hi ) - p.Camera.xyz );
                            const GrassFieldShare at = GrassFieldShareAt( nearest, p.Cull.x, p.Cull.y, p.Spread.w );
                            float share = min( at.Kept, at.Floor );
                            keep        = count;
                            if ( share < 1.0 )
                            {
                                uint below = 0u;
                                uint above = count;
                                while ( below < above )
                                {
                                    uint middle = ( below + above ) / 2u;
                                    float fade  =
                                         unpackUnorm2x16( TilePoolData[TileRecordAt( header, first + middle ) * kTilePoolWords].w ).x;
                                    if ( fade < share )
                                        below = middle + 1u;
                                    else
                                        above = middle;
                                }
                                keep = below;
                            }
                        }
                    }
                    s_Keep[lane]  = keep;
                    s_First[lane] = first;
                }
                barrier();
                uint total = 0u;
                for ( uint c = 0u; c < kTileClusters; ++c )
                    total += s_Keep[c];
                for ( uint t = lane; t < total; t += 64u )
                {
                    uint c = 0u;
                    uint i = t;
                    while ( i >= s_Keep[c] )
                    {
                        i -= s_Keep[c];
                        ++c;
                    }
                    CullCandidate( LoadRecord( TileRecordAt( header, s_First[c] + i ) ), p );
                }
            }
        }
    }
}

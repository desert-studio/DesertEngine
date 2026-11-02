// DesertAsset {"Kind":"Shader","Guid":"bfb6f16ce052412cb6dc16e913525849","Versions":{"SHDR":1},"Dependencies":[]}
// Landscape grass, generated around the camera every frame and never stored (GRASS-GPU; UE: ALandscapeProxy::
// UpdateGrass + FAsyncGrassBuilder::Build, LandscapeGrass.cpp:2378-2520, its grid branch, ported to one thread per
// grid cell). The grid is WORLD-aligned (cell = sqrt(1e6 / Density) cm), so a cell grows the same instance from
// every camera position; its seed is a hash of the cell. An instance is kept when its bilinear layer weight lies in
// AllowedDensityRange and beats the cell's random draw (UE's RandomStream.GetFraction), when it is inside the view's
// side planes, and - past StartCullDistance - when its fade draw beats the distance share: (Start / d)^2, a blade's
// screen area (GRASS-DENSITY), times UE's fade to zero at EndCullDistance - and a kept blade widens by that share and
// lowers over the band's last fifth, a kept mesh shrinks to nothing before its draw drops it (GPU-COST-29), UE's
// (and past the distance D where its mean blade stands MinScreenWidth pixels wide, (D / d)^2 kept and each card
// lifted to that width - GRASS-WIDTH, no sub-pixel strip),
// PerInstanceFadeAmount as its grass materials spend it (scale to nothing at EndCullDistance), so the field thins and
// lowers into the ground instead of ending at a line. Kept instances are appended to their bucket of Instances (one
// mat4 each, the InstanceTransforms of Mesh/Surface/Vertex_Instanced.glslh / Vertex_GrassBlade.glslh): a mesh has one
// bucket per level of its LOD chain, chosen by the instance's screen size (UE grass HISM), a blade one per level of
// its strip, chosen by distance (GRASS-BLADE). A blade also follows its clump - the
// nearest of a jittered grid of clump points (GoT's Voronoi clumps) lends it its facing, height and tint, and pulls it
// towards itself by ClumpPull, so blades gather into tufts with open ground between them instead of standing on the
// jittered grid (white of the grid's own blue noise). Scatter moves a blade past its own cell and Patchiness thins
// the field by broad world patches, so a carpet stops reading as one evenly spaced stalk per cell. FacingJitter turns
// each blade's facing back off its clump's and Tilt stands its root off the vertical in a direction of its own, so
// near blades stop reading as one upright stalk with one lean. GrassFinalize copies each bucket's count into its draw
// records.
// OCCLUSION PER VIEW (GRASS-VIS; UE Nanite / the GPU instance cull: two-phase HZB occlusion, the shadow views and the
// ray-traced scene culled by their own frusta, never by the camera's depth). Instances / Args hold every kept
// instance: the cascades and the TLAS draw them, so grass the camera cannot see still casts and reflects.
// GENERATED ONCE, CULLED EVERY FRAME (GRASS-VIS-24; UE: a landscape component's grass is built once and kept,
// ALandscapeProxy::UpdateGrass, and only its instances are culled per view each frame): the generation (Hzb.w = 0,
// one thread per cell) writes Instances, Args and the cascades' lists, and runs only when its inputs change
// (GrassRenderer VarietyGpu::GenerateKey). The camera's own list, CameraIds / CameraArgs (the same bucket table), is
// a separate pass over those kept instances (CameraCull, one thread per KEPT instance, dispatched indirect by the
// kept count - GrassFinalize CullDispatch; GRASS-CULL-1, UE InstanceCulling: the cull's cost follows the instances,
// not the buffers' capacity): Hzb.w = 3 by the camera's side planes, Hzb.w = 1 by them and the previous frame's
// furthest HZB (Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb, the instance cull's test, through that frame's
// ViewProjection) - every frame, since it depends on last frame's depth; a hidden one becomes a candidate (its
// sphere and its row of Instances). Both lists are appended to with one atomic per subgroup and counter
// (WaveAppend).
// THE LISTS HOLD IDS (GPU-COST-22; UE GPU Scene: an instance's data lies once in
// InstanceSceneData, each view's culling writes the ids of its visible instances, InstanceIdBuffer): an instance is
// written ONCE, into its row of Instances; the camera's list and every cascade's hold that row (one uint each), and
// the vertex stage reads Instances through the id (DESERT_SURFACE_INSTANCE_IDS, Vertex_Instanced.glslh /
// Vertex_GrassBlade.glslh). A list's slot keeps the list's own bucket table, so the slot picks the draw record and
// the strip level, the id the instance. Phase 2 (Hzb.w = 2, one thread per candidate, dispatched indirect by the
// candidates the camera cull appended - GrassFinalize CullDispatch -, after this frame's furthest HZB is
// built from the phase-1 depth) re-tests the candidates through this frame's ViewProjection and appends the ones
// now visible to the camera list after phase 1's; GrassFinalize's post mode writes their draw records (PostArgs);
// a frame whose own HZB was not built (Hzb.z = 0 in phase 2) appends every candidate untested, so a missing HZB
// never drops grass. THE TWO VOLUMES (GRASS-VIS-6; UE: the casters of a whole-scene shadow are culled by the
// cascade's volume swept along the light, not by the camera's side planes): Planes bound the full list - the
// camera's side planes, each moved out along every shadowed directional light by that light's caster reach, so an
// instance outside the frame whose shadow falls into it is kept - and CameraPlanes the camera's own list (the cull
// pass's). While the two differ the camera always has its own list (culled with Hzb.w = 3 without an HZB to test, 1
// with one); with no list of its own (the same volume, no HZB) the camera draws the full list.
// GRASS-CULL-2: the camera cull and phase 2 described above are GrassCull.shader's - one dispatch over every
// variety through the variety table (Common/GrassVarietyRows.glslh), a program of its own (UE InstanceCulling); this
// program is the generation alone, one dispatch per variety over its cells.
Shader "GrassGenerate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GrassFieldShare.glslh>

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
                             // a blade's far LOD: z = the distance at which its bounding radius at scale 1 stands at the
                             // Foliage quality's LodScreenPixels (0: no far LOD; GrassFieldShare.glslh GrassBladeLodAt)
            vec4  Patches;   // the variety's world patches: x = Coverage (1: none), y = Size cm, z = Softness, w = seed bits
            vec4  CardWidths[2]; // a blade's cards' widths, cm at scale 1: card i at [i / 4][i % 4], Buckets.z cards
            uvec4 Cascades;  // the cascades' lists (GPU-COST-21): x = lists (0: none), y = words of one list's copy of
                             // Args' layout in CascadeArgs, z = the cull pad (float bits, cm: a mesh's sway past its bounds)
            vec4  CascadePlanes[48]; // list k's caster box at 6k..6k+5 (GrassRenderer kGrassCascadeLists = 8 lists)
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
        Buffer(8) CascadeIds
        {
            uint GrassCascadeIdsData[];
        };

        Buffer(9) CascadeArgs
        {
            uint CascadeArguments[];
        };

        // Mirrors Graphic::System::GrassGeneratePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            ivec4 CellRange; // x, y = the first cell (world grid index), z, w = cells along x, z (the lens
                             // footprint's box)
        };

        const uint kFlagRandomRotation = 1u;
        const uint kFlagAlignToSurface = 2u;
        const uint kFlagWeightAttenuates = 4u;
        const uint kFlagBlade = 8u;
        const float kNoHeight = -1.0e30;

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

        void main()
        {
            if ( gl_GlobalInvocationID.x >= uint( CellRange.z ) || gl_GlobalInvocationID.y >= uint( CellRange.w ) )
                return;
            // The generation's own copy of the params (the camera cull, GrassCull.shader, makes none).
            GrassParams p = GrassParamsData[0];
            ivec2 cell  = CellRange.xy + ivec2( gl_GlobalInvocationID.xy );
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
                return;
            if ( thinDraw >= p.ScaleZ.w && thinDraw >= LensShare( world, height, p ) )
                return;
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
                return;

            vec3  position = vec3( world.x, height, world.y );
            float distance = length( position - p.Camera.xyz );
            if ( distance > p.Cull.y )
                return;
            // The share of the field kept at this distance (GRASS-DENSITY, Common/GrassFieldShare.glslh - the one
            // law the ground's far cover reads too): beyond StartCullDistance S it falls with the square of the
            // distance, (S / d)^2, so the instances a band of ground spends stay those it spent at S (UE's grass
            // HISM keeps the full density out to its cull distance and spends it on sub-pixel instances) - and on
            // top of that UE's fade, down to nothing at EndCullDistance. Nested: an instance kept at d is kept
            // nearer, so walking does not reshuffle the field, only thins it; before S every instance is kept.
            float                 fade  = Fraction( state );
            const GrassFieldShare field = GrassFieldShareAt( distance, p.Cull.x, p.Cull.y, p.Spread.w );
            float                 kept  = field.Kept;
            // The screen-width floor (GRASS-WIDTH; GoT / UE landscape grass LOD): past D the mean blade would stand
            // narrower than MinScreenWidth pixels, so the field keeps (D / d)^2 of its blades - the law the thinning
            // above widens by (a survivor widened 1/sqrt(share): d / D here) - and each kept blade's own card is
            // lifted to the floor width (below), so no blade strip goes sub-pixel. The same nested draw: a blade
            // kept at d is kept nearer.
            if ( fade >= min( kept, field.Floor ) )
                return;

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
                if ( fade >= min( kept, field.Floor ) * lod.Share )
                    return;
                scale.x *= field.Widen;
                scale.z *= field.Widen;
                scale.y *= field.Height;
            }
            else
                scale *= GrassMeshShrink( field, fade );
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
                scale.y *= mix( 1.0, clumpHeight, p.Blade.z );
                draws = vec3( Fraction( state ), Fraction( state ), mix( Fraction( state ), clumpTint, p.Blade.z ) );
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
                    uint at   = k * p.Cascades.y + table;
                    uint slot = atomicAdd( CascadeArguments[at], 1u );
                    if ( slot < CascadeArguments[at + 2u] )
                        GrassCascadeIdsData[CascadeArguments[at + 1u] + slot] = index;
                }
            }
        }
    }
}

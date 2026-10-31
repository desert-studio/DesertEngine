// DesertAsset {"Kind":"Shader","Guid":"bfb6f16ce052412cb6dc16e913525849","Versions":{"SHDR":1},"Dependencies":[]}
// Landscape grass, generated around the camera every frame and never stored (GRASS-GPU; UE: ALandscapeProxy::
// UpdateGrass + FAsyncGrassBuilder::Build, LandscapeGrass.cpp:2378-2520, its grid branch, ported to one thread per
// grid cell). The grid is WORLD-aligned (cell = sqrt(1e6 / Density) cm), so a cell grows the same instance from
// every camera position; its seed is a hash of the cell. An instance is kept when its bilinear layer weight lies in
// AllowedDensityRange and beats the cell's random draw (UE's RandomStream.GetFraction), when it is inside the view's
// side planes, and - past StartCullDistance - when its fade draw beats the distance share: (Start / d)^2, a blade's
// screen area (GRASS-DENSITY), times UE's fade to zero at EndCullDistance - and a kept one widens by that share and
// lowers over the band's last fifth, UE's
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
// instance: the cascades and the TLAS draw them, so grass the camera cannot see still casts and reflects. When the
// view binds an HZB (Hzb.w = 1) the camera's own list, CameraInstances / CameraArgs (the same bucket table), gets
// only those the previous frame's furthest HZB does not hide (Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb,
// the instance cull's test, through that frame's ViewProjection); a hidden one becomes a candidate (its sphere and
// its row of Instances). Phase 2 (Hzb.w = 2, one thread per candidate slot, after this frame's furthest HZB is
// built from the phase-1 depth) re-tests the candidates through this frame's ViewProjection and appends the ones
// now visible to the camera list after phase 1's; GrassFinalize's post mode writes their draw records (PostArgs);
// a frame whose own HZB was not built (Hzb.z = 0 in phase 2) appends every candidate untested, so a missing HZB
// never drops grass. THE TWO VOLUMES (GRASS-VIS-6; UE: the casters of a whole-scene shadow are culled by the
// cascade's volume swept along the light, not by the camera's side planes): Planes bound the full list - the
// camera's side planes, each moved out along every shadowed directional light by that light's caster reach, so an
// instance outside the frame whose shadow falls into it is kept - and CameraPlanes the camera's own list. While
// the two differ the camera always has its own list (Hzb.w = 3 without an HZB to test, 1 with one); Hzb.w = 0 only
// when they are the same volume, and the camera draws the full list.
Shader "GrassGenerate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GpuCullTests.glslh>
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
            vec4  Lod;       // a mesh's levels by screen size: x = the lens's screen multiple x local half-diagonal, y = |Projection[2][3]|
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

        Uniform sampler PointClampSampler;
        // Phase 1: the previous frame's furthest HZB of the view; phase 2: this frame's (neutral: none tested).
        Uniform(4) texture2D GrassHzbTexture;

        // The camera's list (Hzb.w != 0): the same layout as Instances / Args, then per bucket four words -
        // the candidates appended, the candidates of this frame and phase 1's drawn count (both frozen by
        // GrassFinalize), and phase 2's appended count (GrassRenderer.cpp kPhaseWords).
        Buffer(5) CameraInstances
        {
            mat4 GrassCameraInstancesData[];
        };

        Buffer(6) CameraArgs
        {
            uint CameraArguments[];
        };

        // Per instance slot two uvec4: the hidden instance's sphere (xyz centre, w radius, as float bits) and its
        // row of Instances (x), at the bucket's first slot + the candidate's index.
        Buffer(7) Candidates
        {
            uvec4 GrassCandidatesData[];
        };

        // THE CASCADES' LISTS (GPU-COST-21; UE: every shadow view culls its casters by its own volume): per live
        // cascade view a list of the instances inside that cascade's caster box, the same records and bucket table
        // as Args, list k's copy at k x Cascades.y words, its buckets at k x capacity in CascadeInstances. The
        // cascade draws its list instead of the full one (MeshRenderer::BuildGpuInstancedShadowDraws).
        Buffer(8) CascadeInstances
        {
            mat4 GrassCascadeInstancesData[];
        };

        Buffer(9) CascadeArgs
        {
            uint CascadeArguments[];
        };

        // Mirrors Graphic::System::GrassGeneratePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            ivec4 CellRange;         // phase 1: x, y = the first cell (world grid index), z, w = cells along x, z
                                     // (the lens footprint's box); phase 2: x = candidate slots, y = groups along x
            mat4  HzbViewProjection; // unjittered, what GrassHzbTexture was built through (phase 1 the previous frame's)
            uvec4 Hzb;               // xy = GrassHzbTexture's mip-0 size, z = its mip count (0 in phase 2: none
                                     // built this frame, every candidate is visible), w = the mode: 0 one list,
                                     // 1 the camera's list by its planes and the HZB, 2 phase 2, 3 the camera's
                                     // list by its planes only
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

        // Phase 2: one thread per instance slot of the camera's list. A slot holding one of this frame's candidates
        // (below its bucket's frozen candidate count) whose sphere this frame's furthest HZB no longer hides appends
        // its instance after phase 1's drawn ones.
        void PostOcclusion( GrassParams p )
        {
            uint slot = ( gl_WorkGroupID.y * uint( CellRange.y ) + gl_WorkGroupID.x ) * 64u + gl_LocalInvocationIndex;
            if ( slot >= uint( CellRange.x ) )
                return;
            uint phases = p.Buckets.y + p.Buckets.x * 4u;
            for ( uint b = 0u; b < p.Buckets.x; ++b )
            {
                uint table = p.Buckets.y + b * 4u;
                uint first = Arguments[table + 1u];
                if ( slot < first || slot >= first + Arguments[table + 2u] )
                    continue;
                uint phase = phases + b * 4u;
                if ( slot - first >= CameraArguments[phase + 1u] )
                    return;
                vec4        sphere = uintBitsToFloat( GrassCandidatesData[2u * slot] );
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                if ( Hzb.z != 0u &&
                     GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection,
                                              sphere.xyz - vec3( sphere.w ), sphere.xyz + vec3( sphere.w ) ) )
                    return;
                uint index = CameraArguments[phase + 2u] + atomicAdd( CameraArguments[phase + 3u], 1u );
                if ( index >= Arguments[table + 2u] )
                    return;
                GrassCameraInstancesData[first + index] =
                     GrassInstancesData[GrassCandidatesData[2u * slot + 1u].x];
                return;
            }
        }

        void main()
        {
            GrassParams p = GrassParamsData[0];
            if ( Hzb.w == 2u )
            {
                PostOcclusion( p );
                return;
            }
            if ( gl_GlobalInvocationID.x >= uint( CellRange.z ) || gl_GlobalInvocationID.y >= uint( CellRange.w ) )
                return;
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
            const GrassFieldShare field = GrassFieldShareAt( distance, p.Cull.x, p.Cull.y );
            float                 kept  = field.Kept;
            // The screen-width floor (GRASS-WIDTH; GoT / UE landscape grass LOD): past D the mean blade would stand
            // narrower than MinScreenWidth pixels, so the field keeps (D / d)^2 of its blades - the law the thinning
            // above widens by (a survivor widened 1/sqrt(share): d / D here) - and each kept blade's own card is
            // lifted to the floor width (below), so no blade strip goes sub-pixel. The same nested draw: a blade
            // kept at d is kept nearer.
            float floorShare = GrassThinShare( distance, p.Spread.w );
            if ( fade >= min( kept, floorShare ) )
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
            // Coverage kept through the thinning (GoT / UE-style distance thinning; GrassFieldShare): a survivor
            // stands for the 1/share blades dropped around it, so its width grows by 1/sqrt(share) - at its own
            // height the field keeps sqrt(share) of its covered area, and the ground's far cover takes on the rest
            // (LandscapeGrassCover, GrassFieldCoverage); the height drops only over the last 20 % of the range.
            scale.x *= field.Widen;
            scale.z *= field.Widen;
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
            scale.y *= field.Height;

            // Inside the full list's volume (the camera's side planes swept along the lights), by the instance's
            // bounding sphere; the camera's own list also inside the camera's.
            float radius = p.Range.w * max( max( scale.x, floorBound ), max( scale.y, scale.z ) );
            vec3  centre = position + vec3( 0.0, p.ScaleY.z * scale.y, 0.0 );
            bool  inCamera = true;
            for ( int i = 0; i < 4; ++i )
            {
                if ( dot( p.Planes[i].xyz, centre ) + p.Planes[i].w < -radius )
                    return;
                if ( dot( p.CameraPlanes[i].xyz, centre ) + p.CameraPlanes[i].w < -radius )
                    inCamera = false;
            }
            // Behind what the previous frame's depth held, by the sphere's box: kept for the cascades and the TLAS,
            // a phase-2 candidate for the camera.
            bool hidden = false;
            if ( Hzb.w == 1u && inCamera )
            {
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                hidden = GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection,
                                                  centre - vec3( radius ), centre + vec3( radius ) );
            }

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
            if ( p.Blade.x > 0.0 )
                bucket = min( uint( distance / p.Blade.x ), p.Buckets.x - 1u );
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
                        GrassCascadeInstancesData[CascadeArguments[at + 1u] + slot] = instance;
                }
            }

            // The camera's list: the visible instance at once, the hidden one as a candidate for phase 2. Both
            // counts together stay within the bucket's kept count, so neither passes its capacity.
            if ( ( Hzb.w != 1u && Hzb.w != 3u ) || !inCamera )
                return;
            uint first = Arguments[table + 1u];
            if ( !hidden )
            {
                uint seen = atomicAdd( CameraArguments[table], 1u );
                if ( seen < Arguments[table + 2u] )
                    GrassCameraInstancesData[first + seen] = instance;
                return;
            }
            uint candidate = atomicAdd( CameraArguments[p.Buckets.y + p.Buckets.x * 4u + bucket * 4u], 1u );
            if ( candidate < Arguments[table + 2u] )
            {
                GrassCandidatesData[2u * ( first + candidate )]      = floatBitsToUint( vec4( centre, radius ) );
                GrassCandidatesData[2u * ( first + candidate ) + 1u] = uvec4( index, 0u, 0u, 0u );
            }
        }
    }
}

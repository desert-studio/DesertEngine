// DesertAsset {"Kind":"Shader","Guid":"bfb6f16ce052412cb6dc16e913525849","Versions":{"SHDR":1},"Dependencies":[]}
// Landscape grass, generated around the camera every frame and never stored (GRASS-GPU; UE: ALandscapeProxy::
// UpdateGrass + FAsyncGrassBuilder::Build, LandscapeGrass.cpp:2378-2520, its grid branch, ported to one thread per
// grid cell). The grid is WORLD-aligned (cell = sqrt(1e6 / Density) cm), so a cell grows the same instance from
// every camera position; its seed is a hash of the cell. An instance is kept when its bilinear layer weight lies in
// AllowedDensityRange and beats the cell's random draw (UE's RandomStream.GetFraction), when it is inside the view's
// side planes, and - between StartCullDistance and EndCullDistance - when its fade draw beats the distance share
// (the density falls to zero at EndCullDistance) - and a kept one shrinks by the same share, UE's
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
// near blades stop reading as one upright stalk with one lean. GrassFinalize
// copies each bucket's count into its draw records.
Shader "GrassGenerate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        // Mirrors Graphic::System::GrassParamsGpu (GrassRenderer.hpp), std430.
        struct GrassParams
        {
            vec4  Planes[4]; // the view's left / right / bottom / top planes, xyz.n + w >= 0 inside
            vec4  Camera;    // xyz = view position (cm), w = EndCullDistance through the lens
            vec4  Cull;      // x, y = Start/EndCullDistance through the lens (DrawDistanceScale), z = cell (cm), w = max jitter (cells)
            vec4  Range;     // x/y = AllowedDensityRange, z = MaxScaleWeightAttenuation, w = bounds radius (local)
            vec4  ScaleX;    // x/y = ScaleX range, z = scaling (0 uniform, 1 free, 2 lock XY), w = flags
            vec4  ScaleY;    // x/y = ScaleY range, z = bounds centre height (local)
            vec4  ScaleZ;    // x/y = ScaleZ range (up), w = the floor of the lens share (1 / DrawDistanceScale, at most 1; LensShare)
            uvec4 Info;      // x = layer, y = capacity, z = submesh count, w = seed
            uvec4 Buckets;   // x = bucket count, y = the bucket table's first word in Args
            vec4  Blade;     // x = LodDistance through the lens (0: one bucket), y = ClumpSize cm, z = Clumping, w = ClumpPull
            vec4  Spread;    // x = Scatter (cells a blade may stray past its own), y = Patchiness, z = PatchSize cm
            vec4  Lean;      // x = Tilt (radians), y = FacingJitter (radians), z = the world's SnowDepth (cm)
            vec4  Lod;       // a mesh's levels by screen size: x = the lens's screen multiple x local half-diagonal, y = |Projection[2][3]|
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

        PushConstant PushConstants
        {
            ivec4 CellRange; // x, y = the first cell (world grid index), z, w = cells along x, z (the lens footprint's box)
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
            GrassParams p = GrassParamsData[0];
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
            if ( !( weight > p.Range.x && weight <= p.Range.y && patchy >= keep ) )
                return;

            vec3  position = vec3( world.x, height, world.y );
            float distance = length( position - p.Camera.xyz );
            if ( distance > p.Cull.y )
                return;
            float fade = Fraction( state );
            float faded = clamp( ( distance - p.Cull.x ) / max( p.Cull.y - p.Cull.x, 1.0 ), 0.0, 1.0 );
            if ( fade < faded )
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
            // Coverage kept through the thinning (GoT / UE-style distance thinning): a survivor stands for the
            // 1/share blades dropped around it, so its footprint grows by 1/sqrt(share) and the field's covered
            // area stays the same; the height drops only over the last 20 % of the range, so the far edge melts
            // into the ground instead of the whole band shrinking.
            float share = max( 1.0 - faded, 0.05 );
            float widen = inversesqrt( share );
            scale.x *= widen;
            scale.z *= widen;
            scale.y *= 1.0 - clamp( ( faded - 0.8 ) / 0.2, 0.0, 1.0 );

            // Inside the view's side planes, by the instance's bounding sphere.
            float radius = p.Range.w * max( scale.x, max( scale.y, scale.z ) );
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
            if ( blade )
                GrassInstancesData[index] = mat4( vec4( basis[0] * scale.x, draws.x ), vec4( basis[1] * scale.y, draws.y ),
                                           vec4( basis[2] * scale.y, draws.z ), vec4( position, 1.0 ) );
            else
                GrassInstancesData[index] = mat4( vec4( basis[0] * scale.x, 0.0 ), vec4( basis[1] * scale.y, 0.0 ),
                                           vec4( basis[2] * scale.z, 0.0 ), vec4( position, 1.0 ) );
        }
    }
}

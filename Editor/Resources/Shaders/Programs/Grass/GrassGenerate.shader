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
// bucket, a blade one per level of its strip, chosen by distance (GRASS-BLADE). A blade also follows its clump - the
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
            vec4  Camera;    // xyz = view position (cm), w = EndCullDistance
            vec4  Cull;      // x = StartCullDistance, y = EndCullDistance, z = cell (cm), w = max jitter (cells)
            vec4  Range;     // x/y = AllowedDensityRange, z = MaxScaleWeightAttenuation, w = bounds radius (local)
            vec4  ScaleX;    // x/y = ScaleX range, z = scaling (0 uniform, 1 free, 2 lock XY), w = flags
            vec4  ScaleY;    // x/y = ScaleY range, z = bounds centre height (local)
            vec4  ScaleZ;    // x/y = ScaleZ range (up)
            uvec4 Info;      // x = layer, y = capacity, z = submesh count, w = seed
            uvec4 Buckets;   // x = bucket count, y = the bucket table's first word in Args
            vec4  Blade;     // x = LodDistance (0: one bucket), y = ClumpSize cm, z = Clumping, w = ClumpPull
            vec4  Spread;    // x = Scatter (cells a blade may stray past its own), y = Patchiness, z = PatchSize cm
            vec4  Lean;      // x = Tilt (radians), y = FacingJitter (radians)
        };

        ReadBuffer(0) Field
        {
            uint u_Field[]; // GrassFieldLayout (LandscapeGrassField.hpp)
        };

        ReadBuffer(1) Params
        {
            GrassParams u_Params[];
        };

        Buffer(2) Instances
        {
            mat4 u_Instances[];
        };

        Buffer(3) Args
        {
            uint u_Args[]; // GpuInstances.hpp: header, the draw records, then per bucket count, first, capacity, 0
        };

        PushConstant PushConstants
        {
            ivec4 u_Cells; // x, y = the first cell (world grid index), z = cells per side
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

        uint SamplesX() { return u_Field[0]; }
        uint SamplesZ() { return u_Field[1]; }
        float HeightAt( uint x, uint z ) { return uintBitsToFloat( u_Field[8u + z * SamplesX() + x] ); }
        float WeightAt( uint layer, uint x, uint z )
        {
            uint count = SamplesX() * SamplesZ();
            uint index = z * SamplesX() + x;
            uint word  = u_Field[8u + count + layer * ( ( count + 3u ) / 4u ) + index / 4u];
            return float( ( word >> ( 8u * ( index & 3u ) ) ) & 0xffu ) * ( 1.0 / 255.0 );
        }

        // Bilinear height and layer weight at world XZ; false outside the field or over a missing tile.
        bool SampleField( vec2 world, uint layer, out float height, out float weight )
        {
            vec2  origin  = vec2( uintBitsToFloat( u_Field[4] ), uintBitsToFloat( u_Field[5] ) );
            float spacing = uintBitsToFloat( u_Field[6] );
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
            float spacing = uintBitsToFloat( u_Field[6] );
            float hx, hz, w;
            if ( !SampleField( world + vec2( spacing, 0.0 ), layer, hx, w ) )
                hx = centre;
            if ( !SampleField( world + vec2( 0.0, spacing ), layer, hz, w ) )
                hz = centre;
            return normalize( vec3( centre - hx, spacing, centre - hz ) );
        }

        void main()
        {
            GrassParams p = u_Params[0];
            if ( gl_GlobalInvocationID.x >= uint( u_Cells.z ) || gl_GlobalInvocationID.y >= uint( u_Cells.z ) )
                return;
            ivec2 cell  = u_Cells.xy + ivec2( gl_GlobalInvocationID.xy );
            uint  state = Hash( uint( cell.x ) * 0x8da6b343u ^ uint( cell.y ) * 0xd8163841u ^ p.Info.w );

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
            scale *= 1.0 - faded;

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
            uint table = p.Buckets.y + bucket * 4u;
            uint index = atomicAdd( u_Args[table], 1u );
            if ( index >= u_Args[table + 2u] )
                return;
            index += u_Args[table + 1u];
            // A blade's forward axis carries the height multiplier (its lean is a share of its height) and the
            // columns' w its three draws (Vertex_GrassBlade.glslh); a mesh's are its scaled axes.
            if ( blade )
                u_Instances[index] = mat4( vec4( basis[0] * scale.x, draws.x ), vec4( basis[1] * scale.y, draws.y ),
                                           vec4( basis[2] * scale.y, draws.z ), vec4( position, 1.0 ) );
            else
                u_Instances[index] = mat4( vec4( basis[0] * scale.x, 0.0 ), vec4( basis[1] * scale.y, 0.0 ),
                                           vec4( basis[2] * scale.z, 0.0 ), vec4( position, 1.0 ) );
        }
    }
}

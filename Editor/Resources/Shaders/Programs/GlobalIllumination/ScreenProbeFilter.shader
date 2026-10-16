// DesertAsset {"Kind":"Shader","Guid":"7548c0e5784f417ba082f07320812ea7","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 3/4 — FILTER (UE ScreenProbeFilterGatherTracesCS + ScreenProbeComputeIrradianceCS /
// ScreenProbeConvertToIrradianceCS, LumenScreenProbeFiltering.usf): one workgroup per probe, one thread per texel.
//   spatial   each texel gathers the same direction from the uniform probes of the 3x3 tiles around the probe:
//             a neighbour counts when it lies on this probe's plane (relative to the depth) and the point its ray
//             found is seen from THIS probe within SP_FILTER_MAX_ANGLE of the direction (UE
//             r.Lumen.ScreenProbeGather.SpatialFilterMaxRadianceHitAngle) — a neighbour's ray that hit a near
//             wall this probe does not see adds nothing;
//   radiance  the filtered texels, with a one-texel border repeating the hemi-octahedron's edge (bilinear reads
//             stay in the probe): the rough reflections' source;
//   irradiance second-order SH of the filtered radiance, each texel weighted by its solid angle, convolved with
//             the cosine and / pi: the diffuse source;
//   backface  the irradiance (/ pi) arriving at the probe from the hemisphere behind its surface, -N: the light a
//             two-sided leaf lets through (UE r.Lumen.ScreenProbeGather.TwoSidedFoliageBackfaceDiffuse, which
//             evaluates the full-sphere probe at -N). This probe is a hemisphere around +N and holds nothing behind
//             it, so the back hemisphere is read from the world radiance cache, the full-sphere probes its own rays
//             end in (RadianceCacheIrradianceAt at -N, DDGI-visibility weighted): one read per probe.
Shader "ScreenProbeFilter"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        ReadBuffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform(4) sampler2D u_ScreenProbeTraced;
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D u_ScreenProbeFiltered;
        WriteBuffer(6) ScreenProbeSHUB
        {
            vec4 screenProbeSH[];
        };
        ReadBuffer(7) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(8) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(9) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(10) sampler2D u_RadianceCacheIrradiance;
        Uniform(11) sampler2D u_RadianceCacheDepth;
        WriteBuffer(12) ScreenProbeBackfaceUB
        {
            vec4 screenProbeBackface[]; // rgb = irradiance / pi from -N, a = 1 when the cache answered
        };
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        #define SP_FILTER_MAX_ANGLE_COS 0.98480775 // cos 10 degrees

        shared vec3  s_Radiance[SP_RES * SP_RES];
        shared vec3  s_Direction[SP_RES * SP_RES];
        shared float s_SolidAngle[SP_RES * SP_RES];

        vec3 TexelDirection( mat3 frame, vec2 texel )
        {
            return frame * SPHemiOctDecode( texel / float( SP_RES ) * 2.0 - 1.0 );
        }

        void main()
        {
            const uint probe         = gl_WorkGroupID.x;
            const uint uniformProbes = screenProbeParams.grid.z;
            if ( probe >= uniformProbes &&
                 probe - uniformProbes >= min( screenProbeIndex[0], screenProbeParams.grid.w ) )
                return;
            const ScreenProbe state = screenProbes[probe];
            if ( state.position.w <= 0.0 )
                return;

            const uint  texel  = gl_LocalInvocationIndex;
            const ivec2 t      = ivec2( texel % SP_RES, texel / SP_RES );
            const mat3  frame  = SPProbeFrame( state.normal.xyz );
            const vec3  dir    = TexelDirection( frame, vec2( t ) + 0.5 );

            // ---- Spatial filter over the uniform probes of the 3x3 tiles around this one (this one included).
            const vec4  own    = texelFetch( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t, 0 );
            vec3        sum    = own.rgb;
            float       weight = 1.0;
            const vec3  p      = state.position.xyz;
            const ivec2 tile   = SPUniformTileOf( SPProbePixelOf( state ) );
            for ( int y = -1; y <= 1; ++y )
                for ( int x = -1; x <= 1; ++x )
                {
                    const uint other = SPUniformProbeAt( tile + ivec2( x, y ) );
                    if ( other == SP_INVALID || other == probe )
                        continue;
                    const ScreenProbe neighbour = screenProbes[other];
                    const float plane = abs( dot( neighbour.position.xyz - p, state.normal.xyz ) ) / state.position.w;
                    if ( exp2( -10000.0 * plane * plane ) < 0.5 || dot( dir, neighbour.normal.xyz ) <= 0.0 )
                        continue;
                    const mat3 neighbourFrame = SPProbeFrame( neighbour.normal.xyz );
                    const vec4 ray = texelFetch( u_ScreenProbeTraced,
                                                 SPAtlasOrigin( other, SP_RES ) + SPProbeTexel( neighbourFrame, dir ), 0 );
                    const vec3 hit = neighbour.position.xyz + dir * ray.a;
                    if ( dot( normalize( hit - p ), dir ) < SP_FILTER_MAX_ANGLE_COS )
                        continue;
                    sum    += ray.rgb;
                    weight += 1.0;
                }
            const vec3 radiance = sum / weight;

            // Solid angle of the texel: the area its four corners span on the sphere.
            const vec3 c00 = TexelDirection( frame, vec2( t ) );
            const vec3 c10 = TexelDirection( frame, vec2( t ) + vec2( 1.0, 0.0 ) );
            const vec3 c01 = TexelDirection( frame, vec2( t ) + vec2( 0.0, 1.0 ) );
            const vec3 c11 = TexelDirection( frame, vec2( t ) + vec2( 1.0, 1.0 ) );
            s_Radiance[texel]   = radiance;
            s_Direction[texel]  = dir;
            s_SolidAngle[texel] = 0.5 * ( length( cross( c10 - c00, c01 - c00 ) ) + length( cross( c10 - c11, c01 - c11 ) ) );

            // ---- The filtered radiance with its border.
            const ivec2 tileOrigin = SPAtlasOrigin( probe, SP_TILE ) + 1;
            imageStore( u_ScreenProbeFiltered, tileOrigin + t, vec4( radiance, 1.0 ) );
            barrier();
            // The 4 * (SP_RES + 1) border texels, one per thread: the rows above and below (corners included),
            // then the columns left and right; each repeats the nearest interior texel.
            if ( texel < 4u * ( SP_RES + 1u ) )
            {
                const int R = int( SP_RES );
                const int k = int( texel );
                ivec2     b;
                if ( k < 2 * ( R + 2 ) )
                    b = ivec2( k % ( R + 2 ) - 1, k < R + 2 ? -1 : R );
                else
                {
                    const int j = k - 2 * ( R + 2 );
                    b           = ivec2( j < R ? -1 : R, j % R );
                }
                const ivec2 s = clamp( b, ivec2( 0 ), ivec2( R - 1 ) );
                imageStore( u_ScreenProbeFiltered, tileOrigin + b, vec4( s_Radiance[s.x + s.y * R], 1.0 ) );
            }

            // ---- Irradiance SH: nine coefficients, one thread each.
            if ( texel < 9u )
            {
                vec3  c     = vec3( 0.0 );
                float total = 0.0;
                for ( uint i = 0u; i < SP_RES * SP_RES; ++i )
                {
                    c     += s_Radiance[i] * ( SPSHBasis( texel, s_Direction[i] ) * s_SolidAngle[i] );
                    total += s_SolidAngle[i];
                }
                // The texels cover the hemisphere: their solid angles sum to 2 pi up to the corner chords.
                c *= 6.28318531 / max( total, 1e-4 );
                screenProbeSH[probe * 9u + texel] = vec4( c * SPSHCosineBand( texel ), 0.0 );
            }
            // ---- Backface irradiance: one thread.
            else if ( texel == 9u )
            {
                const vec3  toCamera = giFrame.camera.xyz - p;
                const vec3  v        = toCamera / max( length( toCamera ), 1e-3 );
                const vec4  cache    = RadianceCacheIrradianceAt( p, -state.normal.xyz, v, 0.5 );
                screenProbeBackface[probe] = cache.a > 0.0 ? vec4( cache.rgb, 1.0 ) : vec4( 0.0 );
            }
        }
    }
}

// DesertAsset {"Kind":"Shader","Guid":"2b3d9fdfd8d6428baf3fd72a1d02f1f3","Versions":{"SHDR":1},"Dependencies":[]}
// Ported from UE 5.8 Shaders/Private/Lumen/LumenScreenProbeFiltering.usf ScreenProbeFilterGatherTracesCS /
// GatherNeighborRadiance (r.Lumen.ScreenProbeGather.SpatialFilterProbes, run SpatialFilterNumPasses = 3 times by
// GlobalIllumination::ScreenProbeGather), adapted:
//   * the position weight is UE's exp2(-SpatialFilterPositionWeightScale * x²) of the neighbour's distance from
//     THIS probe's plane relative to the probe's depth, not of the depth difference (UE's test lets a neighbour on
//     a grazing floor through with the weight of one on a wall facing the camera);
//   * the probes are hemi-octahedra around their own normals (ScreenProbeGather.hpp): a neighbour answers a
//     direction only if its hemisphere holds it, through its own frame (SPProbeTexel);
//   * no strong filter for moving traces (UE ScreenProbeMoving: the gather does not measure it, REMAINDER).
// THE SCREEN PROBE GATHER, SPATIAL FILTER: one workgroup per probe, one thread per traced texel. Each texel gathers
// the same direction from the uniform probes of the four neighbouring tiles: weighted by how near the neighbour
// lies to this probe's plane and by the angle, seen from THIS probe, to the point the neighbour's ray hit (its hit
// distance clamped to this texel's own, which keeps contact shadows) — linear to zero at
// SP_SPATIAL_FILTER_MAX_RADIANCE_HIT_ANGLE (UE SpatialFilterMaxRadianceHitAngle). A disoccluded probe
// (ScreenProbeDisocclusion) drops the angle test: the temporal filter cannot help there. Radiance in rgb, the
// texel's own hit distance carried through in a for the next pass.
Shader "ScreenProbeSpatialFilter"
{
    Compute
    {

        #include <Common/ScreenProbes.glslh>
        // One thread per traced texel of a probe (SP_RES², the layout permutation).
        LocalSize(SP_TRACED_TEXELS, SP_TRACED_TEXELS, 1);

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
        ReadBuffer(3) ScreenProbeDisocclusionUB
        {
            float screenProbeDisocclusion[];
        };
        Uniform sampler PointClampSampler;
        Uniform(4) texture2D ScreenProbeRadiance; // rgb = radiance, a = hit distance (cm), SP_RES² per probe
        #define ScreenProbeRadiance sampler2D(ScreenProbeRadiance, PointClampSampler)
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D FilteredScreenProbeRadiance;
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #include <Common/ScreenProbeTemporal.glslh>

        // UE r.Lumen.ScreenProbeGather.SpatialFilterPositionWeightScale and SpatialFilterMaxRadianceHitAngle (10
        // degrees, in radians).
        #define SP_SPATIAL_FILTER_POSITION_WEIGHT_SCALE 1000.0
        #define SP_SPATIAL_FILTER_MAX_RADIANCE_HIT_ANGLE 0.17453293

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

            const ivec2 t           = ivec2( gl_LocalInvocationID.xy );
            const ivec2 at          = SPAtlasOrigin( probe, SP_RES ) + t;
            const mat3  frame       = SPProbeFrame( state.normal.xyz );
            const vec3  dir         = frame * SPHemiOctDecode( ( vec2( t ) + 0.5 ) / float( SP_RES ) * 2.0 - 1.0 );
            const vec4  own         = texelFetch( ScreenProbeRadiance, at, 0 );
            const float hitDistance = own.a;
            const bool  relaxAngle  = screenProbeDisocclusion[probe] > SP_DISOCCLUSION_FRACTION;
            const vec3  p           = state.position.xyz;
            const ivec2 tile        = SPUniformTileOf( SPProbePixelOf( state ) );

            vec3  sum    = own.rgb;
            float weight = 1.0;
            const ivec2 offsets[4] = ivec2[4]( ivec2( -1, 0 ), ivec2( 1, 0 ), ivec2( 0, -1 ), ivec2( 0, 1 ) );
            for ( int i = 0; i < 4; ++i )
            {
                const uint other = SPUniformProbeAt( tile + offsets[i] );
                if ( other == SP_INVALID || other == probe )
                    continue;
                const ScreenProbe neighbour = screenProbes[other];
                if ( neighbour.position.w <= 0.0 || dot( dir, neighbour.normal.xyz ) <= 0.0 )
                    continue;
                const float plane          = abs( dot( neighbour.position.xyz - p, state.normal.xyz ) ) / state.position.w;
                const float positionWeight = exp2( -SP_SPATIAL_FILTER_POSITION_WEIGHT_SCALE * plane * plane );
                const vec4  ray            = texelFetch( ScreenProbeRadiance,
                                                         SPAtlasOrigin( other, SP_RES ) +
                                                              SPProbeTexel( SPProbeFrame( neighbour.normal.xyz ), dir ),
                                                         0 );
                float angleWeight = 1.0;
                if ( !relaxAngle )
                {
                    const vec3  toHit = neighbour.position.xyz + dir * min( ray.a, hitDistance ) - p;
                    const float angle = acos( clamp( dot( toHit, dir ) / max( length( toHit ), 1e-4 ), -1.0, 1.0 ) );
                    angleWeight       = 1.0 - clamp( angle / SP_SPATIAL_FILTER_MAX_RADIANCE_HIT_ANGLE, 0.0, 1.0 );
                }
                const float w = positionWeight * angleWeight;
                sum += ray.rgb * w;
                weight += w;
            }
            imageStore( FilteredScreenProbeRadiance, at, vec4( sum / weight, hitDistance ) );
        }
    }
}

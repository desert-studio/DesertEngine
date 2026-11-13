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
// THE SCREEN PROBE GATHER, SPATIAL FILTER: one workgroup per probe, one thread per traced texel, each the one pass
// of Common/ScreenProbeSpatialFilter.glslh SPSpatialFilterTexel (the weights). GlobalIllumination::ScreenProbeGather
// runs it for every pass but the last; the last runs inside ScreenProbeFilter.shader (SP_FILTER_SPATIAL), the only
// reader of its output, on its own probe (FPS-GI-FARFIELD).
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

        #include <Common/ScreenProbeSpatialFilter.glslh>

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

            const ivec2 t = ivec2( gl_LocalInvocationID.xy );
            imageStore( FilteredScreenProbeRadiance, SPAtlasOrigin( probe, SP_RES ) + t,
                        SPSpatialFilterTexel( probe, state, t ) );
        }
    }
}

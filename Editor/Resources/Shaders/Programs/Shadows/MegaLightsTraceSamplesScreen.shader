// DesertAsset {"Kind":"Shader","Guid":"103f958d30124324a0d54e5aebfae0ae","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamplesScreen"
{
    // "MegaLights: Trace Samples" on a device with neither ray query nor virtual shadow maps
    // (StochasticVisibility::Screen): each surviving sample's light tested once from the representative surface - its
    // contact shadow, then the screen trace over the closest HZB; what the screen does not hold casts no shadow.
    // The body is Common/MegaLightsTraceSamples.glslh, shared with the ray-query and virtual programs.

    Compute
    {
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D HzbTexture; // the closest-depth HZB of the G-buffer depth (HZBRenderer), every mip
        #define HzbTexture sampler2D(HzbTexture, PointClampSampler)
        #include <Common/HZBTrace.glslh>
        #define MEGALIGHTS_VISIBILITY_SCREEN
        #include <Common/MegaLightsTraceSamples.glslh>
    }
}

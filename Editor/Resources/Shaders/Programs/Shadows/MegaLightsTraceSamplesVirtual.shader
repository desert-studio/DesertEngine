// DesertAsset {"Kind":"Shader","Guid":"7eea23009f9e4d8fb240b18877324394","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamplesVirtual"
{
    // "MegaLights: Trace Samples" without ray tracing, on a device with virtual shadow maps
    // (StochasticVisibility::VirtualMapsAndScreen; UE MegaLights' screen traces, then the light's VSM): each surviving
    // sample's light tested once from the representative surface - its contact shadow, the screen trace over the
    // closest HZB, then the light's virtual shadow map pages ("VSM: Mark Local" marked them from this G-buffer).
    // The body is Common/MegaLightsTraceSamples.glslh, shared with the ray-query and screen programs.

    Compute
    {
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D HzbTexture; // the closest-depth HZB of the G-buffer depth (HZBRenderer), every mip
        #define HzbTexture sampler2D(HzbTexture, PointClampSampler)
        #include <Common/HZBTrace.glslh>
        #include <Common/VirtualShadowMap.glslh>
        // The VSM reader's tables (Core::kVsmReadFrameDataBinding .. kVsmReadLocalLightsBinding, as
        // Mesh/ForwardLocalLights.glslh spells them), bound by name (BindVirtualShadowMapReads).
        layout( std430, binding = 45 ) restrict readonly buffer VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        layout( std430, binding = 46 ) restrict readonly buffer VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        layout( binding = 47, r32ui ) restrict readonly uniform uimage2D PhysicalPoolTexture;
        layout( std430, binding = 48 ) restrict readonly buffer VsmLocalLightsUB
        {
            VsmLocalLight vsmLocalLights[];
        };
        #define DESERT_VSM_READ
        #include <Common/VirtualShadowMap.glslh>
        #define MEGALIGHTS_VISIBILITY_SCREEN
        #define MEGALIGHTS_VISIBILITY_VSM
        #include <Common/MegaLightsTraceSamples.glslh>
    }
}

// DesertAsset {"Kind":"Shader","Guid":"e2d31ed8e7ba417d8ffeb9b6be0c4b74","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamplesRayQueryVirtual"
{
    // "MegaLights: Trace Samples" on a ray-query frame with VirtualMaps lamps (and maybe Screen ones): each sample
    // tests its light by the payload's ShadowVisibility - the ray query (0), or the screen trace over the closest
    // HZB then, with a VSM slot, the light's virtual shadow map pages. Body: Common/MegaLightsTraceSamples.glslh.
    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        Uniform sampler PointClampSampler;
        Uniform(18) texture2D HzbTexture; // the closest-depth HZB (HZBRenderer), every mip; 18: the TLAS holds 7
        #define HzbTexture sampler2D(HzbTexture, PointClampSampler)
        #include <Common/HZBTrace.glslh>
        #include <Common/VirtualShadowMap.glslh>
        // The VSM reader's tables (Core::kVsmReadFrameDataBinding .. kVsmReadLocalLightsBinding), bound by name
        // (BindVirtualShadowMapReads).
        layout( std430, binding = 49 ) restrict readonly buffer VsmFrameData
        {
            VsmFrame Frame;
            VsmLevel Levels[];
        };
        layout( std430, binding = 50 ) restrict readonly buffer VsmPageTable
        {
            VsmPageEntry Entries[];
        };
        layout( binding = 51, r32ui ) restrict readonly uniform uimage2D PhysicalPoolTexture;
        layout( std430, binding = 52 ) restrict readonly buffer VsmLocalLightsUB
        {
            VsmLocalLight vsmLocalLights[];
        };
        #define DESERT_VSM_READ
        #include <Common/VirtualShadowMap.glslh>
        #define MEGALIGHTS_VISIBILITY_RAY_QUERY
        #define MEGALIGHTS_VISIBILITY_SCREEN
        #define MEGALIGHTS_VISIBILITY_VSM
        #include <Common/MegaLightsTraceSamples.glslh>
    }
}

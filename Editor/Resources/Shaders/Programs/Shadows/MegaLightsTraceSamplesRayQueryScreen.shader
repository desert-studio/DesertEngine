// DesertAsset {"Kind":"Shader","Guid":"72418e0bb7814c3d8ce51064da638386","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamplesRayQueryScreen"
{
    // "MegaLights: Trace Samples" on a ray-query frame with Screen lamps (a light whose Shadow Method resolved to
    // Screen, Graphic::ResolveLightVisibility): each sample tests its light by the payload's ShadowVisibility - the
    // ray query over the TLAS (0), or the screen trace over the closest HZB. Body: Common/MegaLightsTraceSamples.glslh.
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
        #define MEGALIGHTS_VISIBILITY_RAY_QUERY
        #define MEGALIGHTS_VISIBILITY_SCREEN
        #include <Common/MegaLightsTraceSamples.glslh>
    }
}

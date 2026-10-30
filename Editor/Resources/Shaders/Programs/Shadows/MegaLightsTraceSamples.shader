// DesertAsset {"Kind":"Shader","Guid":"c335ed30c6694ec88f30e63116010664","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamples"
{
    // "MegaLights: Trace Samples" (UE MegaLightsHardwareRayTracing + its screen traces): THE ONE RAY of each sample.
    // Per sample pixel, after the temporal and spatial reuse, the light every reservoir ended with is tested once
    // from the sample pixel's representative surface (Common/MegaLights.glslh MegaLightsVisibility): its contact
    // shadow through the G-buffer depth, then one ray query over the view's TLAS (masked segments alpha-tested)
    // toward a point of the light's source sphere. The visibility scales the reservoir's W (0 = occluded: it keeps
    // its confidence and loses its weight, RTXDI visibility reuse). These reservoirs are both what every render
    // pixel of the block shades ("MegaLights: Shade Samples", no ray) and next frame's temporal history, with the
    // representative's surface beside them.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        Uniform sampler PointClampSampler;
        #define MEGALIGHTS_VISIBILITY_RAY_QUERY
        #include <Common/MegaLightsTraceSamples.glslh>
    }
}

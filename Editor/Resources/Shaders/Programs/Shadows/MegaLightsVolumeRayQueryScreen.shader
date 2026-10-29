// DesertAsset {"Kind":"Shader","Guid":"4e04997d096b4287871473fdc5823355","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolumeRayQueryScreen"
{
    // "MegaLights: Volume" on a ray-query frame with Screen lamps (and no VirtualMaps one): each froxel sample tests
    // its light by the payload's ShadowVisibility - the ray query (0), or the screen trace from the froxel point over
    // the closest HZB (2, Screen; off the screen unshadowed), the surface's MegaLightsTraceSamplesRayQueryScreen
    // pair. Body: Common/MegaLightsVolumeWrite.glslh.
    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        #define MEGALIGHTS_VOLUME_VISIBILITY_RAY_QUERY
        #define MEGALIGHTS_VOLUME_VISIBILITY_SCREEN
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

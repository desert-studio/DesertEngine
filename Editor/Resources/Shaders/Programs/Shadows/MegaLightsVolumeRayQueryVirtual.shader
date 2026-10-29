// DesertAsset {"Kind":"Shader","Guid":"b718788552fa49c49747e0aa0e005c5c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolumeRayQueryVirtual"
{
    // "MegaLights: Volume" on a ray-query frame with VirtualMaps lamps (Screen lamps beside): each froxel sample
    // tests its light by the payload's ShadowVisibility - the ray query (0); the screen trace from the froxel point
    // over the closest HZB, then the light's virtual shadow map pages (1); the screen trace alone (2, Screen).
    // Body: Common/MegaLightsVolumeWrite.glslh.
    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
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
        #define MEGALIGHTS_VOLUME_VISIBILITY_RAY_QUERY
        #define MEGALIGHTS_VOLUME_VISIBILITY_SCREEN
        #define MEGALIGHTS_VOLUME_VISIBILITY_VSM
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

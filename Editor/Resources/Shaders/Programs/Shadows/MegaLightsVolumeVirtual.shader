// DesertAsset {"Kind":"Shader","Guid":"85d48e60b2064bcfb511ea4efac16332","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolumeVirtual"
{
    // "MegaLights: Volume" without ray tracing, on a device with virtual shadow maps
    // (StochasticVisibility::VirtualMapsAndScreen, Screen lamps beside): each froxel sample's light marched from the
    // froxel point over the closest HZB (the surface's screen trace), then, a vsm+screen light, tested against its
    // virtual shadow map pages (VSM-LOCAL), the same pages MegaLights' surface trace reads; a froxel no G-buffer pixel
    // marked reads the light's coarse mip. The body is Common/MegaLightsVolumeWrite.glslh, shared with the ray-query
    // and screen programs.

    Compute
    {
        #include <Common/VirtualShadowMap.glslh>
        // The VSM reader's tables (Core::kVsmReadFrameDataBinding .. kVsmReadLocalLightsBinding, as
        // Mesh/ForwardLocalLights.glslh spells them), bound by name (BindVirtualShadowMapReads).
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
        #define MEGALIGHTS_VOLUME_VISIBILITY_SCREEN
        #define MEGALIGHTS_VOLUME_VISIBILITY_VSM
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

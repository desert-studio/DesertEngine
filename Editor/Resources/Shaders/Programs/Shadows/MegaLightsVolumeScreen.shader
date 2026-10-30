// DesertAsset {"Kind":"Shader","Guid":"f74320a22cc14402ac9a47c6a16f7f56","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolumeScreen"
{
    // "MegaLights: Volume" on a device with neither ray query nor virtual shadow maps (StochasticVisibility::Screen):
    // each froxel sample inside the frustum marches the closest HZB from the froxel point toward its light (the
    // surface's screen trace, Common/MegaLightsScreenTrace.glslh); off the screen it is unshadowed (UE MegaLights'
    // volume takes no screen trace at all). The body is Common/MegaLightsVolumeWrite.glslh, shared with the
    // ray-query and virtual programs.

    Compute
    {
        #define MEGALIGHTS_VOLUME_VISIBILITY_SCREEN
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

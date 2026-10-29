// DesertAsset {"Kind":"Shader","Guid":"f74320a22cc14402ac9a47c6a16f7f56","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolumeScreen"
{
    // "MegaLights: Volume" on a device with neither ray query nor virtual shadow maps (StochasticVisibility::Screen):
    // the froxel samples are unshadowed - a froxel is not on the screen, so it has no screen trace (UE MegaLights'
    // volume takes none either). The body is Common/MegaLightsVolumeWrite.glslh, shared with the ray-query and
    // virtual programs.

    Compute
    {
        #define MEGALIGHTS_VOLUME_VISIBILITY_NONE
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

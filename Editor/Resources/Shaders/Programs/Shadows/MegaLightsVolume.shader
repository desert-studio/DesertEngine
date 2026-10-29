// DesertAsset {"Kind":"Shader","Guid":"30f0dda90e7646fea408519be79bdf7e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVolume"
{
    // "MegaLights: Volume" (UE 5.5 MegaLightsVolume: the translucency volume MegaLights lights). Per FROXEL of the
    // view's frustum (Shadows.MegaLightsVolumeTilePixels square tiles x Shadows.MegaLightsVolumeSlices logarithmic
    // slices, Common/MegaLightsVolume.glslh), at a point jittered inside the froxel every frame:
    //  1) resampled importance sampling over the light-grid cell of that point (every light when the cell
    //     overflowed): Shadows.MegaLightsVolumeSamples lights drawn in proportion to their unshadowed radiance's
    //     luminance there (the point has no surface: no cosine, no BRDF);
    //  2) one visibility ray per drawn light (ray query over the view's TLAS, masked segments alpha-tested) toward a
    //     point of the light's source sphere (SourceRadius: soft shadows over frames), as MegaLights' surface traces;
    //  3) the shadowed estimate projected on L1 spherical harmonics (L0, L1 x y z per colour channel);
    //  4) blended with the previous frame's volume at the point's reprojected froxel (an exponential average of
    //     1 / (Shadows.MegaLightsMaxHistory + 1)), unless the point was outside it or the history is unreadable.
    // The result is next frame's history and what the forward and translucent cells read. This is the ray-query
    // program (StochasticVisibility::RayQuery); the body is Common/MegaLightsVolumeWrite.glslh, shared with
    // MegaLightsVolumeVirtual.shader (vsm+screen) and MegaLightsVolumeScreen.shader (screen).

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        #define MEGALIGHTS_VOLUME_VISIBILITY_RAY_QUERY
        #include <Common/MegaLightsVolumeWrite.glslh>
    }
}

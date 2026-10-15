// DesertAsset {"Kind":"Shader","Guid":"a83f26d1c95e4b07b2d4e6f10c3a8b52","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VolumetricFogIntegration"
{
    Compute
    {
        // FRONT-TO-BACK INTEGRATION of the lit froxels (UE VolumetricFog.usf FinalIntegrationCS, reimplemented).
        // One thread per froxel COLUMN walks its slices from the camera outward and stores, in slice z, what lies
        // between the camera and the FAR edge of that slice: .rgb = in-scattered radiance (premultiplied),
        // .a = transmittance. Programs/Fog/HeightFog.shader then needs one trilinear fetch at a pixel's depth and
        // composes `scene = rgb + scene * a`, exactly as it composes the analytic fog.
        //
        // Each slice is integrated analytically for a constant medium (Hillaire 2015, energy-conserving):
        // S * (1 - e^(-ext * len)) / ext, so a slice of dense fog cannot add more light than it received - the
        // naive S * len does, and brightens the near slices as the grid gets coarser.

        #include <Common/VolumetricFog.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image3D u_IntegratedLightScattering;
        Uniform(2) sampler3D u_LightScattering;

        LocalSize(8, 8, 1);

        void main()
        {
            uvec2 column = gl_GlobalInvocationID.xy;
            if (any(greaterThanEqual(column, u_VfGridSize.xy)))
                return;

            vec3  rayDir;
            VolumetricFogWorldPosition(vec2(column) + 0.5, 1.0, rayDir);
            // Ray length per centimetre of view depth along this column.
            float lengthPerDepth = 1.0 / max(dot(rayDir, u_VfCameraForward.xyz), 1e-4);

            vec3  accumulated   = vec3(0.0);
            float transmittance = 1.0;
            // Slice 0's near edge is the camera's near plane, not the camera; the fog in front of it is counted
            // with slice 0's medium (the march starts at depth 0) so a camera inside dense fog has no clear bubble.
            float nearDepth     = 0.0;
            for (uint z = 0u; z < u_VfGridSize.z; ++z)
            {
                float farDepth  = VolumetricFogDepthFromSlice(float(z + 1u));
                float stepLen   = (farDepth - nearDepth) * lengthPerDepth;
                vec4  medium    = texelFetch(u_LightScattering, ivec3(column, z), 0);
                float ext       = max(medium.a, 1e-7);
                float stepTrans = exp(-ext * stepLen);
                accumulated += transmittance * medium.rgb * (1.0 - stepTrans) / ext;
                transmittance *= stepTrans;
                imageStore(u_IntegratedLightScattering, ivec3(column, z), vec4(accumulated, transmittance));
                nearDepth = farDepth;
            }
        }
    }
}

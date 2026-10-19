// DesertAsset {"Kind":"Shader","Guid":"b9daf08be2a0453cad0ce278ec7ac6a1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureDiffuse"
{
    Compute
    {
        // THE CAPTURE'S SIDE OF THE NORMALISATION (UE ComputeMixingWeight, Common/ReflectionEnvironment.glslh),
        // stored in the alpha of the capture's LAST mip - the capture's ONE stored brightness, which the lighting
        // divides the pixel's own diffuse by. What it is follows the view's estimator
        // (Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp ReflectionCaptureLighting):
        //
        //   THE RADIANCE CACHE (u_Estimator.x = 1): the CUBE'S OWN brightness, UE's AverageBrightness - the last
        // mip's radiance about each axis, the roughest lobe the filter wrote, i.e. the cube's light as a surface
        // facing that axis would gather it. The pixel's side is the cache's smooth irradiance (Mesh/AmbientIBL.glslh),
        // so the ratio rescales the cube to the GI's brightness where it is read: a cube rendered without the GI
        // (a face view runs none) no longer passes its own darkness - or a lamp seen directly - into every rough
        // surface it covers (REFL-NORM-SMOOTH: the Bistro night measured against the cache at the capture point
        // laid a grey veil over the street, 46.6 against 53.1 without captures; its own brightness gives 54.7).
        //   THE SKY (u_Estimator.x = 0): the sky's irradiance at the capture point, as the pixels receive it. Not the
        // cube's own: a pixel lit by the sky alone at night would divide a lamp-lit cube's brightness by the dark
        // sky - the black reflections UE shows without lightmaps.
        //
        // WHAT IS STORED: a luminance per axis normal - an AMBIENT CUBE: the last mip is 1x1 per face, so face f's
        // texel holds the value about f's axis and the lighting reads it about the pixel's own normal. (UE stores
        // one value, the cube's mean radiance; the per-axis form keeps the direction it throws away.)
        //
        // ONE INVOCATION PER LISTED CUBE. The cube being written this frame is measured unconditionally (its old
        // value belongs to the old cube); any other only while its stored value is 0, "not measured".

        LocalSize(1, 1, 1);

        #include <Common/SkyLook.glslh>

        Uniform sampler u_LinearClampSampler;
        Uniform(5) textureCube u_EnvIrradianceTex; // the sky's irradiance cube, as the view's pixels read it
        #define u_EnvIrradianceTex samplerCube(u_EnvIrradianceTex, u_LinearClampSampler)
        struct DiffusePoint
        {
            vec4  Point;      // xyz = the capture point (world cm)
            uvec4 CubeForced; // x = cube index in the array (the staging cube for a recapture), y = 1 measure
                              // unconditionally (the cube being written)
        };
        ReadBuffer(6) ReflectionCaptureDiffusePointsUB
        {
            DiffusePoint u_Points[];
        };
        layout(binding = 7, rgba16f) uniform imageCubeArray u_Cubes; // the array's LAST mip: 1x1 per face

        PushConstant PushConstants
        {
            vec4 u_Estimator;    // x = 1 the radiance cache, 0 the sky's irradiance; y = the point count
            vec4 u_SkyYawCosSin; // Graphic::SkyLookGPU: how the view reads its sky cubes
            vec4 u_SkyGain;
        };

        void main()
        {
            const uint index = gl_GlobalInvocationID.x;
            if (index >= uint(u_Estimator.y))
                return;
            const DiffusePoint point = u_Points[index];
            const int firstLayer = int(point.CubeForced.x) * 6;

            if (point.CubeForced.y == 0u)
            {
                bool measured = true;
                for (int i = 0; i < 6; ++i)
                    measured = measured && imageLoad(u_Cubes, ivec3(0, 0, firstLayer + i)).a > 0.0;
                if (measured)
                    return;
            }

            const vec3 axes[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1),
                                         vec3(0, 0, -1));
            for (int i = 0; i < 6; ++i)
            {
                const ivec3 texel = ivec3(0, 0, firstLayer + i);
                const vec4  own   = imageLoad(u_Cubes, texel);
                const vec3  light = u_Estimator.x > 0.5
                                         ? own.rgb
                                         : ApplySkyGain(texture(u_EnvIrradianceTex,
                                                                SkyLookDirection(axes[i], u_SkyYawCosSin.xy)).rgb,
                                                        u_SkyGain.rgb);
                imageStore(u_Cubes, texel, vec4(own.rgb, dot(light, vec3(0.2126, 0.7152, 0.0722))));
            }
        }
    }
}

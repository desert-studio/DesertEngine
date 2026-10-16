// DesertAsset {"Kind":"Shader","Guid":"b9daf08be2a0453cad0ce278ec7ac6a1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureDiffuse"
{
    Compute
    {
        // THE DIFFUSE AT THE CAPTURE POINTS, measured by the estimator the view's lit pixels are normalised with
        // (Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp ReflectionCaptureLighting), and stored in the
        // alpha of the capture's LAST mip - the capture's ONE stored brightness, which the lighting divides the
        // pixel's own diffuse by (UE ComputeMixingWeight, Common/ReflectionEnvironment.glslh).
        //
        // WHAT IS MEASURED: the luminance of the irradiance (/ pi, the convention of the sky's irradiance cube and of
        // the GI's final gather) about each of the six axis normals - an AMBIENT CUBE of the capture point: the last
        // mip is 1x1 per face, so face f's texel holds the value about f's axis and the lighting reads it about the
        // pixel's own normal, comparing like with like in direction as well as in estimator. (UE stores one value,
        // the cube's mean radiance - which is also the mean of these six's E(n) / pi over every normal; the per-axis
        // form keeps the direction it throws away.)
        //
        // ONE INVOCATION PER WRITTEN CUBE, every frame. The cube being written this frame is measured
        // unconditionally (its old value belongs to the old cube). Any other cube is measured only while its stored
        // value is 0, "not measured": a cube written before the radiance cache had traced probes around its point
        // (outside the clipmaps, or the first frames of a scene) is normalised as soon as the cache answers there,
        // not at the next lighting change. A point the estimator does not answer in all six directions stays 0 on
        // every axis - never a guessed value, never a mix of measured and missing axes.

        LocalSize(1, 1, 1);

        #include <Common/RadianceCache.glslh>
        #include <Common/SkyLook.glslh>

        ReadBuffer(0) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(1) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(2) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(3) sampler2D   u_RadianceCacheIrradiance;
        Uniform(4) sampler2D   u_RadianceCacheDepth;
        Uniform(5) samplerCube u_EnvIrradianceTex; // the sky's irradiance cube, as the view's pixels read it
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

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

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
            float luminance[6];
            float answered = 0.0;
            for (int i = 0; i < 6; ++i)
            {
                const vec3 n = axes[i];
                if (u_Estimator.x > 0.5)
                {
                    // The cache's SMOOTH read, the one the pixels take their side of the ratio from
                    // (Mesh/AmbientIBL.glslh DesertCaptureNormalisation): the field without the per-probe
                    // visibility whose cell-shaped steps the ratio once carried onto the walls.
                    const vec4 irradiance = RadianceCacheSmoothIrradianceAt(point.Point.xyz, n);
                    luminance[i] = dot(irradiance.rgb, vec3(0.2126, 0.7152, 0.0722));
                    if (irradiance.a > 0.0)
                        answered += 1.0;
                }
                else
                {
                    const vec3 sky = ApplySkyGain(
                         texture(u_EnvIrradianceTex, SkyLookDirection(n, u_SkyYawCosSin.xy)).rgb, u_SkyGain.rgb);
                    luminance[i] = dot(sky, vec3(0.2126, 0.7152, 0.0722));
                    answered += 1.0;
                }
            }
            // An unanswered re-measure leaves the stored 0 as it is; a forced one stores 0 (the old cube's value
            // does not describe the new one).
            if (answered < 6.0 && point.CubeForced.y == 0u)
                return;
            for (int i = 0; i < 6; ++i)
            {
                const ivec3 texel = ivec3(0, 0, firstLayer + i);
                imageStore(u_Cubes, texel, vec4(imageLoad(u_Cubes, texel).rgb, answered >= 6.0 ? luminance[i] : 0.0));
            }
        }
    }
}

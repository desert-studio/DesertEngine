// DesertAsset {"Kind":"Shader","Guid":"b9daf08be2a0453cad0ce278ec7ac6a1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureDiffuse"
{
    Compute
    {
        // THE DIFFUSE AT ONE CAPTURE POINT, measured by the estimator the view's lit pixels receive their diffuse
        // from (Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp ReflectionCaptureLighting), in the frame the
        // capture's cube is written. ReflectionCaptureFilter stores it in the cube's alpha - the capture's ONE stored
        // brightness - and the lighting divides the pixel's own diffuse by it (UE ComputeMixingWeight,
        // Common/ReflectionEnvironment.glslh).
        //
        // WHAT IS MEASURED: the luminance of the irradiance (/ pi, the convention of the sky's irradiance cube and of
        // the GI's final gather) about each of the six axis normals - an AMBIENT CUBE of the capture point, which the
        // lighting reads about the pixel's own normal, so the ratio compares like with like in direction as well as
        // in estimator. (UE stores one value, the cube's mean radiance - which is also the mean of these six's
        // E(n) / pi over every normal; the per-axis form keeps the direction it throws away.)
        //
        // Record: one vec4 per axis in the cube's face order, x = that luminance, y = the normals the estimator
        // answered. A point the radiance cache does not cover with traced probes in all six directions (outside the
        // clipmaps, or probes not traced yet) is stored as 0 on every axis: "not measured", which the lighting does
        // not normalise by - never a guessed value, never a mix of measured and missing axes.

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
        Buffer(6) ReflectionCaptureDiffuseUB
        {
            vec4 u_Diffuse[6];
        };

        PushConstant PushConstants
        {
            vec4 u_CapturePoint; // xyz = world cm; w = 1 the radiance cache, 0 the sky's irradiance
            vec4 u_SkyYawCosSin; // Graphic::SkyLookGPU: how the view reads its sky cubes
            vec4 u_SkyGain;
        };

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        void main()
        {
            const vec3 axes[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1),
                                         vec3(0, 0, -1));
            float luminance[6];
            float answered = 0.0;
            for (int i = 0; i < 6; ++i)
            {
                const vec3 n = axes[i];
                if (u_CapturePoint.w > 0.5)
                {
                    // A point in the air has no viewer: the read's bias goes along the normal (V = N), as the
                    // radiance cache marks it (RadianceCacheMark.shader's capture points). The dither picks the
                    // middle of a clipmap fade; both ends of it are marked.
                    const vec4 irradiance = RadianceCacheIrradianceAt(u_CapturePoint.xyz, n, n, 0.5);
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
            for (int i = 0; i < 6; ++i)
                u_Diffuse[i] = vec4(answered >= 6.0 ? luminance[i] : 0.0, answered, 0.0, 0.0);
        }
    }
}

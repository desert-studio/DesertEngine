// DesertAsset {"Kind":"Shader","Guid":"0590522cedcb4ee198f06ebf369b0f1b","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER (GIMethod / ReflectionMethod PathTraced; System::PathTracer). The reference image of the view:
// PathTracing.PathsPerFrame paths per pixel per frame through the view's TLAS and the SAME hit groups the hit-lit
// passes use (each surface template's `RayTracing.Hit` cell, the landscape's TerrainRayHit: the payload carries the
// hit's material, this ray generation lights it), summed into the view's accumulation image;
// PathTracerResolve.shader divides by the sample count and replaces the lit scene before post.
//
// Ported from UE 5.8 Engine/Shaders/Private/PathTracing - PathTracingCore.ush (the bounce loop: next event
// estimation with one light chosen by its estimate, the light ray and the material ray combined by the power
// heuristic, Russian roulette on the throughput), Light/PathTracingLightSampling.ush:15-115 (TraceLight /
// SampleLight / EstimateLight per light type), Light/PathTracingDirectionalLight.ush:10-60 (the sun as a cone of
// SourceAngle), Light/PathTracingSkyLight.ush (the sky, Common/PathTracingSky.glslh), MonteCarlo.ush
// MISWeightPower. Adapted: the material is the engine's one lit BRDF (Mesh/DirectLighting.glslh EvaluateDirectLight:
// Lambert + GGX, metallic) sampled as a mixture of a cosine lobe and the GGX distribution of visible normals (Heitz
// 2018) instead of UE's per-shading-model lobes; point and spot lights are the engine's delta sources with their
// authored falloff (Mesh/LightSources.glslh) instead of UE's spherical area lights; a PCG hash sequence instead of
// UE's scrambled Sobol (FRandomSequence); a box pixel filter.
Shader "PathTracer"
{
    Pass "PathTracing"
    {
        RayGen
        {
            #include <Common/CameraUB.glslh>
            #include <Common/GIFrame.glslh>
            #include <Common/SkyLook.glslh>
            #include <Common/ReconstructPosition.glslh>
            #include <Mesh/LightSources.glslh>
            #include <Mesh/DirectLighting.glslh>
            #include <Common/RayTracingPayload.glslh>

            layout(binding = 1) uniform accelerationStructureEXT u_SceneTLAS;
            // The view's running sum (rgb) of every path since the last reset; a = the paths summed.
            layout(binding = 2, rgba32f) uniform image2D u_Accumulation;
            layout(std430, binding = 3) readonly buffer GIFrameUB
            {
                GIFrame giFrame;
            };
            layout(std430, binding = 4) readonly buffer PointLightsUB
            {
                PointLight pointLights[];
            };
            layout(std430, binding = 5) readonly buffer SpotLightsUB
            {
                SpotLight spotLights[];
            };
            layout(binding = 6, rgba32f) readonly uniform image2D u_SkyRadiance;
            layout(std430, binding = 7) readonly buffer PathTracingSkyPdfUB
            {
                float skyPdf[];
            };
            Uniform sampler u_LinearClampSampler;
            Uniform(8) textureCube u_EnvSpecularTex;
            #define u_EnvSpecularTex samplerCube(u_EnvSpecularTex, u_LinearClampSampler)

            #define PATH_TRACING_SKY_RADIANCE u_SkyRadiance
            #define PATH_TRACING_SKY_PDF      skyPdf
            #include <Common/PathTracingSky.glslh>

            layout(location = 0) rayPayloadEXT DesertRayPayload payload;

            PushConstant PushConstants
            {
                uvec4 u_Counts; // x = point lights, y = spot lights, z = paths already summed (0: overwrite), w = max bounces
                uvec4 u_Sky;    // x = sky side S (0: no sky light), y = its pyramid levels, z = frame seed, w = paths this frame (>= 1)
                vec4  u_Sun;    // x = sin of half the sun's source angle (0: a delta direction), y = pixel spread angle
            };

            // ---- Random numbers: a PCG hash chain per pixel and path (UE: FRandomSequence). ----
            uint PathTracingHash(uint v)
            {
                const uint state = v * 747796405u + 2891336453u;
                const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
                return (word >> 22u) ^ word;
            }
            float PathTracingRandom(inout uint rng)
            {
                rng = PathTracingHash(rng);
                return float(rng >> 8u) * (1.0 / 16777216.0);
            }
            vec2 PathTracingRandom2(inout uint rng)
            {
                return vec2(PathTracingRandom(rng), PathTracingRandom(rng));
            }

            float PathTracingLuminance(vec3 c)
            {
                return dot(c, vec3(0.2126, 0.7152, 0.0722));
            }

            // UE MonteCarlo.ush MISWeightPower: the power heuristic, beta = 2.
            float PathTracingMISWeightPower(float pdf, float otherPdf)
            {
                if (pdf >= otherPdf)
                    return 1.0 / (1.0 + (otherPdf / pdf) * (otherPdf / pdf));
                if (pdf > 0.0)
                    return (pdf / otherPdf) * (pdf / otherPdf) / (1.0 + (pdf / otherPdf) * (pdf / otherPdf));
                return 0.0;
            }

            mat3 PathTracingBasis(vec3 n)
            {
                const vec3 t = normalize(abs(n.y) < 0.999 ? cross(vec3(0.0, 1.0, 0.0), n) : cross(vec3(1.0, 0.0, 0.0), n));
                return mat3(t, cross(n, t), n);
            }

            // ---- The material at a hit: the engine's lit BRDF, f * cos for one light direction. ----
            struct PathTracingMaterial
            {
                vec3  BaseColor;
                float Metallic;
                float Roughness;
                vec3  F0;
                float SpecularProbability; // the mixture weight of the GGX lobe in the sampler
            };

            PathTracingMaterial PathTracingMaterialOf(vec3 N, vec3 V)
            {
                PathTracingMaterial m;
                m.BaseColor = payload.BaseColor;
                m.Metallic  = payload.Metallic;
                m.Roughness = max(payload.Roughness, 0.04);
                m.F0        = mix(vec3(0.04), m.BaseColor, m.Metallic);
                // Choose the lobes by their share of the reflected energy at this view (UE picks lobes by their
                // estimated albedo, PathTracingMaterialSampling.ush).
                const float specular = PathTracingLuminance(fresnelSchlick(m.F0, max(dot(N, V), 0.0)));
                const float diffuse  = PathTracingLuminance(m.BaseColor) * (1.0 - m.Metallic) * (1.0 - specular);
                m.SpecularProbability = diffuse > 0.0 ? clamp(specular / (specular + diffuse), 0.05, 0.95) : 1.0;
                return m;
            }

            vec3 PathTracingBsdfCos(PathTracingMaterial m, vec3 N, vec3 V, vec3 L)
            {
                return EvaluateDirectLight(L, vec3(1.0), V, N, m.F0, m.Metallic, m.Roughness, m.BaseColor);
            }

            float PathTracingSmithG1(float NoV, float alpha)
            {
                const float a2 = alpha * alpha;
                return 2.0 * NoV / max(NoV + sqrt(a2 + (1.0 - a2) * NoV * NoV), 1e-6);
            }

            // The sampler's solid-angle density of L: the mixture of the cosine lobe and the visible-normal GGX lobe.
            float PathTracingBsdfPdf(PathTracingMaterial m, vec3 N, vec3 V, vec3 L)
            {
                const float NoL = dot(N, L);
                const float NoV = max(dot(N, V), 1e-4);
                if (NoL <= 0.0)
                    return 0.0;
                const vec3  H        = normalize(V + L);
                const float alpha    = m.Roughness * m.Roughness;
                const float specular = DistributionGGX(max(dot(N, H), 0.0), m.Roughness) *
                                       PathTracingSmithG1(NoV, alpha) / (4.0 * NoV);
                return m.SpecularProbability * specular + (1.0 - m.SpecularProbability) * NoL / PI;
            }

            // Heitz 2018, "Sampling the GGX Distribution of Visible Normals" (UE MonteCarlo.ush
            // ImportanceSampleVisibleGGX): a half vector in the frame of N, seen from Ve.
            vec3 PathTracingSampleVisibleGGX(vec3 Ve, float alpha, vec2 u)
            {
                const vec3  Vh    = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
                const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
                const vec3  T1    = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
                const vec3  T2    = cross(Vh, T1);
                const float r     = sqrt(u.x);
                const float phi   = 2.0 * PI * u.y;
                const float t1    = r * cos(phi);
                const float s     = 0.5 * (1.0 + Vh.z);
                const float t2    = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * r * sin(phi);
                const vec3  Nh    = t1 * T1 + t2 * T2 + sqrt(max(1.0 - t1 * t1 - t2 * t2, 0.0)) * Vh;
                return normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0)));
            }

            vec3 PathTracingSampleBsdf(PathTracingMaterial m, vec3 N, vec3 V, inout uint rng)
            {
                const mat3 basis = PathTracingBasis(N);
                const vec2 u     = PathTracingRandom2(rng);
                if (PathTracingRandom(rng) < m.SpecularProbability)
                {
                    const vec3 Ve = transpose(basis) * V;
                    const vec3 H  = basis * PathTracingSampleVisibleGGX(Ve, m.Roughness * m.Roughness, u);
                    return reflect(-V, H);
                }
                const float r   = sqrt(u.x);
                const float phi = 2.0 * PI * u.y;
                return basis * vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u.x, 0.0)));
            }

            // ---- Rays. ----
            // The material ray: the hit's surface into the payload (HitT < 0: a miss).
            void PathTracingTraceMaterial(vec3 origin, vec3 direction, float coneWidth)
            {
                payload.HitT       = -1.0;
                payload.ConeWidth  = coneWidth;
                payload.ConeSpread = u_Sun.y;
                traceRayEXT(u_SceneTLAS, gl_RayFlagsNoneEXT, 0xFFu, 0u, 0u, 0u, origin, 0.0, direction, 1.0e9, 0);
            }

            // The light ray: any hit (the any-hit stages still cut masked holes) ends it; the miss stage writes
            // HitT < 0, so an unchanged HitT means occluded.
            bool PathTracingVisible(vec3 origin, vec3 direction, float distance)
            {
                payload.HitT = 1.0;
                traceRayEXT(u_SceneTLAS,
                            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFFu, 0u, 0u, 0u,
                            origin, 0.0, direction, distance, 0);
                return payload.HitT < 0.0;
            }

            // ---- The lights (UE PathTracingLightSampling.ush). Index: 0 = the sun, 1 = the sky, 2.. = point lights,
            // then spot lights. ----
            bool PathTracingHasSun()
            {
                return giFrame.sunTravel.w > 0.5;
            }
            // A sky with no light (the black cube of a view without an environment, or a black sky) is no light
            // source: its mean weight is 0, and every pdf of it would divide by that mean.
            bool PathTracingHasSky()
            {
                return u_Sky.x > 0u && PathTracingSkyMean(u_Sky.x, u_Sky.y) > 0.0;
            }

            // The sun's cone: 1 - cos(theta max) and its solid angle (UE DirectionalLight_TraceLight).
            float PathTracingSunOneMinusCos()
            {
                const float s2 = u_Sun.x * u_Sun.x;
                return s2 < 0.01 ? s2 * (0.5 + 0.125 * s2) : 1.0 - sqrt(1.0 - s2);
            }

            // UE EstimateLight: the light's share in the choice at P.
            float PathTracingEstimate(uint light, vec3 P)
            {
                if (light == 0u)
                    return PathTracingHasSun() ? PathTracingLuminance(giFrame.sunRadiance.rgb) : 0.0;
                if (light == 1u)
                    return PathTracingHasSky() ? PathTracingSkyEstimate(u_Sky.x, u_Sky.y) : 0.0;
                const uint i = light - 2u;
                if (i < u_Counts.x)
                    return PathTracingLuminance(DesertPointLightAt(pointLights[i], P).Radiance);
                return PathTracingLuminance(DesertSpotLightAt(spotLights[i - u_Counts.x], P).Radiance);
            }

            float PathTracingEstimateTotal(vec3 P)
            {
                float total = 0.0;
                const uint lights = 2u + u_Counts.x + u_Counts.y;
                for (uint l = 0u; l < lights; ++l)
                    total += PathTracingEstimate(l, P);
                return total;
            }

            // Next event estimation at a hit: one light chosen by its estimate, its sample weighted against the
            // material's density (area lights: the sun's cone, the sky); delta lights take the whole weight.
            vec3 PathTracingSampleLights(PathTracingMaterial m, vec3 P, vec3 N, vec3 V, vec3 origin, float total,
                                         inout uint rng)
            {
                if (total <= 0.0)
                    return vec3(0.0);
                const uint lights = 2u + u_Counts.x + u_Counts.y;
                float      pick   = PathTracingRandom(rng) * total;
                uint       chosen = lights - 1u;
                float      share  = 0.0;
                for (uint l = 0u; l < lights; ++l)
                {
                    share = PathTracingEstimate(l, P);
                    if (pick < share)
                    {
                        chosen = l;
                        break;
                    }
                    pick -= share;
                }
                const float pickPdf = share / total;
                if (pickPdf <= 0.0)
                    return vec3(0.0);
                const vec2 u = PathTracingRandom2(rng);
                if (chosen == 0u)
                {
                    const vec3 toSun = -normalize(giFrame.sunTravel.xyz);
                    vec3       L     = toSun;
                    float      lightPdf = 0.0; // 0: a delta direction, no MIS
                    if (u_Sun.x > 0.0)
                    {
                        // UE UniformSampleConeRobust over the source angle.
                        const float oneMinusCos = PathTracingSunOneMinusCos();
                        const float cosTheta    = 1.0 - u.x * oneMinusCos;
                        const float sinTheta    = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
                        const float phi         = 2.0 * PI * u.y;
                        L        = PathTracingBasis(toSun) * vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
                        lightPdf = 1.0 / (2.0 * PI * oneMinusCos);
                    }
                    if (dot(N, L) <= 0.0 || !PathTracingVisible(origin, L, 1.0e9))
                        return vec3(0.0);
                    // radiance / pdf of the cone sample is the sun's colour (the cone's radiance is normalised by
                    // its solid angle, UE DirectionalLight_SampleLight).
                    const float weight = lightPdf > 0.0
                                             ? PathTracingMISWeightPower(pickPdf * lightPdf, PathTracingBsdfPdf(m, N, V, L))
                                             : 1.0;
                    return PathTracingBsdfCos(m, N, V, L) * giFrame.sunRadiance.rgb * weight / pickPdf;
                }
                if (chosen == 1u)
                {
                    const PathTracingSkySample s = PathTracingSkySampleLight(u, u_Sky.x, u_Sky.y);
                    if (s.Pdf <= 0.0 || dot(N, s.Direction) <= 0.0 || !PathTracingVisible(origin, s.Direction, 1.0e9))
                        return vec3(0.0);
                    const float weight = PathTracingMISWeightPower(pickPdf * s.Pdf, PathTracingBsdfPdf(m, N, V, s.Direction));
                    return PathTracingBsdfCos(m, N, V, s.Direction) * s.Radiance * weight / (pickPdf * s.Pdf);
                }
                const uint  i = chosen - 2u;
                DesertLight light;
                vec3        position;
                if (i < u_Counts.x)
                {
                    light    = DesertPointLightAt(pointLights[i], P);
                    position = pointLights[i].position;
                }
                else
                {
                    light    = DesertSpotLightAt(spotLights[i - u_Counts.x], P);
                    position = spotLights[i - u_Counts.x].position;
                }
                if (dot(N, light.L) <= 0.0 || !PathTracingVisible(origin, normalize(position - origin), distance(position, origin)))
                    return vec3(0.0);
                return PathTracingBsdfCos(m, N, V, light.L) * light.Radiance / pickPdf;
            }

            // What a material ray that left the scene sees: the sky, and the sun's disc when it has a size. Weighted
            // against the light sampler of the vertex it left (lightPickSky / lightPickSun: that vertex's choice
            // probabilities; bsdfPdf < 0: a camera ray, nothing to weigh).
            vec3 PathTracingMissRadiance(vec3 direction, float bsdfPdf, float lightPickSky, float lightPickSun)
            {
                vec3 radiance = vec3(0.0);
                if (bsdfPdf < 0.0)
                    radiance += ApplySkyGain(textureLod(u_EnvSpecularTex,
                                                        SkyLookDirection(direction, giFrame.skyYawCosSin.xy), 0.0).rgb,
                                             giFrame.skyGain.rgb);
                else if (PathTracingHasSky())
                {
                    const vec4 sky = PathTracingSkyEval(direction, u_Sky.x, u_Sky.y);
                    radiance += sky.rgb * PathTracingMISWeightPower(bsdfPdf, lightPickSky * sky.w);
                }
                if (PathTracingHasSun() && u_Sun.x > 0.0)
                {
                    const float oneMinusCos = PathTracingSunOneMinusCos();
                    if (1.0 - dot(direction, -normalize(giFrame.sunTravel.xyz)) < oneMinusCos)
                    {
                        const float solidAngle = 2.0 * PI * oneMinusCos;
                        const float weight =
                            bsdfPdf < 0.0 ? 1.0 : PathTracingMISWeightPower(bsdfPdf, lightPickSun / solidAngle);
                        radiance += giFrame.sunRadiance.rgb / solidAngle * weight;
                    }
                }
                return radiance;
            }

            // One path of the pixel: the camera ray, then up to u_Counts.w bounces. @p sampleIndex is the path's index
            // in the pixel's accumulation (paths summed before it + its place in this frame), so every path of the
            // pixel since the restart draws its own random sequence.
            vec3 PathTracingTracePath(ivec2 pixel, ivec2 size, uint sampleIndex)
            {
                uint rng = PathTracingHash(uint(pixel.x) + PathTracingHash(uint(pixel.y) + PathTracingHash(sampleIndex + PathTracingHash(u_Sky.z))));

                // The camera ray through a uniformly chosen point of the pixel (a box filter).
                const vec2 uv     = (vec2(pixel) + PathTracingRandom2(rng)) / vec2(size);
                vec3       origin = cameraUB.CameraPos;
                vec3       dir    = normalize(ReconstructWorldPosition(uv, 0.5, cameraUB.InvJitteredViewProjection) - origin);

                vec3  radiance     = vec3(0.0);
                vec3  throughput   = vec3(1.0);
                float bsdfPdf      = -1.0; // the density the current ray was drawn with; < 0: the camera ray
                float lightPickSky = 0.0;
                float lightPickSun = 0.0;
                float coneWidth    = 0.0;
                for (uint bounce = 0u;; ++bounce)
                {
                    PathTracingTraceMaterial(origin, dir, coneWidth);
                    if (payload.HitT < 0.0)
                    {
                        radiance += throughput * PathTracingMissRadiance(dir, bsdfPdf, lightPickSky, lightPickSun);
                        break;
                    }
                    const vec3 P = origin + dir * payload.HitT;
                    const vec3 N = payload.Normal;
                    const vec3 V = -dir;
                    // No light is sampled on emissive surfaces, so the material ray takes their whole weight.
                    radiance += throughput * payload.Emissive;

                    const PathTracingMaterial m = PathTracingMaterialOf(N, V);
                    coneWidth += u_Sun.y * payload.HitT;
                    // The next rays leave the surface by an offset that grows with the distance travelled, so they do
                    // not meet the triangle they start on.
                    const vec3  next  = P + N * (0.05 + 1.0e-5 * length(P - cameraUB.CameraPos));
                    const float total = PathTracingEstimateTotal(P);
                    radiance += throughput * PathTracingSampleLights(m, P, N, V, next, total, rng);
                    if (bounce >= u_Counts.w)
                        break;

                    lightPickSky = total > 0.0 ? PathTracingEstimate(1u, P) / total : 0.0;
                    lightPickSun = total > 0.0 ? PathTracingEstimate(0u, P) / total : 0.0;
                    const vec3 L = PathTracingSampleBsdf(m, N, V, rng);
                    bsdfPdf      = PathTracingBsdfPdf(m, N, V, L);
                    if (bsdfPdf <= 0.0)
                        break;
                    throughput *= PathTracingBsdfCos(m, N, V, L) / bsdfPdf;
                    // Russian roulette after the third bounce, on the throughput's largest channel (UE
                    // PathTracingCore.ush: continue with probability min(1, max(throughput))).
                    if (bounce >= 3u)
                    {
                        const float survive = min(max(throughput.r, max(throughput.g, throughput.b)), 1.0);
                        if (PathTracingRandom(rng) >= survive)
                            break;
                        throughput /= survive;
                    }
                    origin = next;
                    dir    = L;
                }
                // A NaN or infinite path would poison every later sample of the pixel: it is dropped (counted as
                // black), as UE's accumulation does.
                if (any(isnan(radiance)) || any(isinf(radiance)))
                    radiance = vec3(0.0);
                return radiance;
            }

            // This frame's u_Sky.w paths of the pixel (PathTracing.PathsPerFrame, cut to what the total
            // PathTracing.SamplesPerPixel leaves), added to the running sum in one read and one write.
            void main()
            {
                const ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
                const ivec2 size  = imageSize(u_Accumulation);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                vec3 sum = vec3(0.0);
                for (uint s = 0u; s < u_Sky.w; ++s)
                    sum += PathTracingTracePath(pixel, size, u_Counts.z + s);
                const vec4 previous = u_Counts.z == 0u ? vec4(0.0) : imageLoad(u_Accumulation, pixel);
                imageStore(u_Accumulation, pixel, vec4(previous.rgb + sum, previous.a + float(u_Sky.w)));
            }
        }

        Miss
        {
            #include <Common/RayTracingPayload.glslh>

            layout(location = 0) rayPayloadInEXT DesertRayPayload payload;

            void main()
            {
                payload.HitT = -1.0;
            }
        }
    }
}

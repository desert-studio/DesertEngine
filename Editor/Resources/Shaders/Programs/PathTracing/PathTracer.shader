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
// 2018) instead of UE's per-shading-model lobes; point and spot lights keep their authored falloff
// (Mesh/LightSources.glslh) but are seen as UE's emitter spheres of SourceRadius (UE SampleSphereLight,
// Common/LightSourceSampling.glslh): the light sample's direction is uniform in the cone the sphere subtends and its
// light ray ends on the sphere, so a lamp whose centre sits inside its bulb's socket still shines through the rest of
// the sphere; the material rays never meet the sphere, so that sample takes the whole weight; a PCG hash sequence instead of
// UE's scrambled Sobol (FRandomSequence); a box pixel filter.
//
// THE EMISSIVE SURFACES AS LIGHTS (Common/EmissiveLights.glslh, the scene's one emissive triangle list): next event
// estimation picks a triangle by its power and a point uniformly on it, as its own strategy beside the analytic
// lights (NVIDIA RTXDI: one sample per light kind), and a material ray that meets emission takes the power-heuristic
// weight against that sampler. The light ray to the point is a material ray: its closest hit is the sampled triangle
// when nothing occludes it, and the hit group returns the material's emission there (a textured emission included).
//
// GLASS (UE PathTracingGlass.ush, the smooth case: the specular reflect/refract pair; PathTracingFresnel.ush
// FresnelDielectric): a BlendMode Translucent surface's hit group writes its transmittance colour and IOR into the
// payload (Common/RayTracingPayload.glslh, the inputs the raster's glass pass reads). There the path picks reflection
// by the exact dielectric Fresnel term and otherwise refracts by the IOR (IOR 1: a thin sheet, straight through and
// tinted at each crossing); inside solid glass the throughput is absorbed by Beer's law, the tint reached after
// kPathTracingAbsorptionDistance. The interface is a delta lobe: no light sample there. Light rays walk the
// translucent primitives (kMaskTranslucent) and are attenuated by them in a straight line instead of blocked (UE's
// shadow any-hit transmittance: no caustics through refraction); a material ray that refracted keeps the MIS state
// of the vertex it left, so it and that vertex's attenuated light sample stay one weighed pair.
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
            #include <Common/LightSourceSampling.glslh>
            #include <Mesh/DirectLighting.glslh>
            #include <Common/RayTracingPayload.glslh>
            #include <Common/EmissiveLights.glslh>

            layout(binding = 1) uniform accelerationStructureEXT SceneTlas;
            // The view's running sum (rgb) of every path since the last reset; a = the paths summed.
            layout(binding = 2, rgba32f) uniform image2D AccumulationTexture;
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
            layout(binding = 6, rgba32f) readonly uniform image2D SkyRadianceTexture;
            layout(std430, binding = 7) readonly buffer PathTracingSkyPdfUB
            {
                float skyPdf[];
            };
            Uniform sampler LinearClampSampler;
            Uniform(8) textureCube EnvironmentSpecularTexture;
            #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)

            // The cloud layer's shadow map (VolumetricCloudRenderer, CloudShadowMapOrWhite) and its block
            // (CloudShadowPackUniforms): the sun's transmittance through the clouds, the receiver text every lit
            // pass compiles (Common/CloudShadowReceiver.glslh).
            Uniform sampler LinearRepeatSampler;
            Uniform(16) texture2D CloudShadowMapTexture;
            #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearRepeatSampler)
            Uniform(17) CloudShadowUB {
                mat4 CloudShadowWorldToMap;
                vec4 CloudShadowParams;
            };
            #include <Common/CloudShadowReceiver.glslh>

            // The scene's emissive triangles (RayTracing::EmissiveLightsFrame).
            layout(std430, binding = 9) readonly buffer EmissiveSegmentsUB
            {
                EmissiveSegment emissiveSegments[];
            };
            layout(std430, binding = 10) readonly buffer EmissiveLookupUB
            {
                uint emissiveLookup[];
            };
            layout(std430, binding = 11) readonly buffer EmissiveTrianglesUB
            {
                EmissiveTriangle emissiveTriangles[];
            };
            layout(std430, binding = 12) readonly buffer EmissivePowerUB
            {
                float emissivePower[];
            };
            // Its dynamic part: the skinned primitives' emissive triangles of this frame's pose.
            layout(std430, binding = 13) readonly buffer EmissiveDynamicSegmentsUB
            {
                EmissiveSegment emissiveDynamicSegments[];
            };
            layout(std430, binding = 14) readonly buffer EmissiveDynamicTrianglesUB
            {
                EmissiveTriangle emissiveDynamicTriangles[];
            };
            layout(std430, binding = 15) readonly buffer EmissiveDynamicPowerUB
            {
                float emissiveDynamicPower[];
            };
            #include <Common/EmissiveLightsSampling.glslh>

            #define PATH_TRACING_SKY_RADIANCE SkyRadianceTexture
            #define PATH_TRACING_SKY_PDF      skyPdf
            #include <Common/PathTracingSky.glslh>

            layout(location = 0) rayPayloadEXT DesertRayPayload payload;

            PushConstant PushConstants
            {
                uvec4 Counts; // x = point lights, y = spot lights, z = paths already summed (0: overwrite), w = max bounces
                uvec4 SkyParams;    // x = sky side S (0: no sky light), y = its pyramid levels, z = frame seed, w = paths this frame (>= 1)
                vec4  SunParams;    // x = sin of half the sun's source angle (0: a delta direction), y = pixel spread angle,
                                    // z = the same as x of atmosphere light 1 (the moon), w = the path intensity
                                    // clamp in radiance (MaxPathIntensity / PreExposure; 0: no clamp)
                uvec4 EmissiveCounts;        // the static part: x = triangles, y = power tree leaves, z = its levels, w = TLAS instances of the lookup
                uvec4 EmissiveDynamicCounts; // the dynamic part: x = triangles, y = power tree leaves, z = its levels
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

            // ---- Glass: the dielectric interface of a translucent hit (Transmission > 0). ----
            // RayTracingScene.hpp kMaskShadowCaster / kMaskTranslucentShadowCaster: the light rays trace only the
            // primitives with CastShadows (UE PATHTRACER_MASK_SHADOW), as the raster's shadow maps and RT shadows
            // do. A primitive with CastShadows off - a street lamp's glass around its own point light - is still
            // met by the material rays (cull mask 0xFF), so it is seen, lit and emits; it only never occludes.
            const uint kPathTracingMaskShadowCaster            = 0x02u;
            const uint kPathTracingMaskTranslucentShadowCaster = 0x08u;
            // Solid glass reaches its tint after this many centimetres inside it (Beer's law).
            const float kPathTracingAbsorptionDistance = 100.0;
            // A light ray crosses at most this many translucent surfaces; past them it is blocked. A bound on the
            // walk's loop only: one ray never meets this many glass sheets in a scene.
            const uint kPathTracingMaxTranslucentLayers = 32u;

            bool PathTracingIsTransmissive()
            {
                return any(greaterThan(payload.Transmission, vec3(0.0)));
            }

            // UE FresnelDielectric: the unpolarised reflectance of a smooth dielectric interface for the cosine
            // cosI on the incident side and eta = n(incident) / n(transmitted); 1 past the critical angle.
            float PathTracingFresnelDielectric(float cosI, float eta)
            {
                const float sin2T = eta * eta * (1.0 - cosI * cosI);
                if (sin2T >= 1.0)
                    return 1.0;
                const float cosT = sqrt(1.0 - sin2T);
                const float rs   = (eta * cosI - cosT) / (eta * cosI + cosT);
                const float rp   = (cosI - eta * cosT) / (cosI + eta * cosT);
                return 0.5 * (rs * rs + rp * rp);
            }

            // The absorption coefficient (1/cm) of solid glass whose transmittance colour is @p tint.
            vec3 PathTracingAbsorption(vec3 tint)
            {
                return -log(clamp(tint, vec3(1.0e-4), vec3(1.0))) / kPathTracingAbsorptionDistance;
            }

            // ---- Rays. ----
            // Both rays index the hit groups per BLAS segment (sbtRecordStride 1): the table holds one record per
            // segment record, an instance's SBT offset is its first one, and a hit's geometry index picks its
            // segment's (RayTracingHitRecord.glslh, as SSRTrace.glslh). Stride 0 shaded every segment of a BLAS
            // with its FIRST segment's group and record: a tree's leaves took the trunk's template (no any-hit, no
            // mask) and the trunk's index range.
            // The material ray: the hit's surface into the payload (HitT < 0: a miss; Transmission stays 0 unless a
            // translucent hit group writes it).
            void PathTracingTraceMaterialMasked(vec3 origin, float tMin, vec3 direction, float coneWidth, float distance,
                                                uint cullMask)
            {
                payload.HitT         = -1.0;
                payload.ConeWidth    = coneWidth;
                payload.ConeSpread   = SunParams.y;
                payload.Transmission = vec3(0.0);
                traceRayEXT(SceneTlas, gl_RayFlagsNoneEXT, cullMask, 0u, 1u, 0u, origin, tMin, direction, distance, 0);
            }
            void PathTracingTraceMaterialTo(vec3 origin, vec3 direction, float coneWidth, float distance)
            {
                PathTracingTraceMaterialMasked(origin, 0.0, direction, coneWidth, distance, 0xFFu);
            }
            void PathTracingTraceMaterial(vec3 origin, vec3 direction, float coneWidth)
            {
                PathTracingTraceMaterialTo(origin, direction, coneWidth, 1.0e9);
            }

            // The light ray's transmittance: 0 when an opaque shadow caster blocks it (any hit of that mask ends
            // the ray; the any-hit stages still cut masked holes, and the miss stage writes HitT < 0, so an unchanged
            // HitT means occluded); otherwise the product over the translucent shadow casters on the segment, walked
            // nearest first: a thin sheet's tint, a solid's Fresnel transmission at each interface and its
            // absorption between entering and leaving it.
            vec3 PathTracingTransmittance(vec3 origin, vec3 direction, float distance)
            {
                payload.HitT = 1.0;
                traceRayEXT(SceneTlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                            kPathTracingMaskShadowCaster, 0u, 1u, 0u, origin, 0.0, direction, distance, 0);
                if (payload.HitT >= 0.0)
                    return vec3(0.0);
                vec3  transmittance = vec3(1.0);
                float start         = 0.0;
                float entered       = 0.0; // where the ray entered the solid it is in (0: it starts inside one)
                for (uint layer = 0u; layer < kPathTracingMaxTranslucentLayers; ++layer)
                {
                    PathTracingTraceMaterialMasked(origin, start, direction, 0.0, distance,
                                                   kPathTracingMaskTranslucentShadowCaster);
                    if (payload.HitT < 0.0)
                        return transmittance;
                    // A translucent primitive with no dielectric to give (an Additive surface) blocks.
                    if (!PathTracingIsTransmissive())
                        return vec3(0.0);
                    const bool front = (payload.Flags & kDesertRayPayloadFrontFace) != 0u;
                    if (payload.Ior <= 1.0)
                        transmittance *= payload.Transmission;
                    else
                    {
                        const float cosI = abs(dot(payload.Normal, direction));
                        transmittance *= 1.0 - PathTracingFresnelDielectric(cosI, front ? 1.0 / payload.Ior : payload.Ior);
                        if (front)
                            entered = payload.HitT;
                        else
                            transmittance *= exp(-PathTracingAbsorption(payload.Transmission) * (payload.HitT - entered));
                    }
                    if (all(lessThanEqual(transmittance, vec3(0.0))))
                        return vec3(0.0);
                    start = payload.HitT + 0.05;
                }
                return vec3(0.0);
            }

            // ---- The lights (UE PathTracingLightSampling.ush). Index: 0 = the sun, 1 = the sky, 2 = atmosphere light 1
            // (the moon), 3.. = point lights, then spot lights. ----
            const uint kPathTracingFirstLocalLight = 3u;

            // The scene's directional lights (UE: every directional light is in the path tracer's light list):
            // d = 0 the sun, d = 1 atmosphere light 1 (the moon); light index 0 and 2.
            bool PathTracingHasDirectional(uint d)
            {
                return (d == 0u ? giFrame.sunTravel.w : giFrame.light1Travel.w) > 0.5;
            }
            vec3 PathTracingDirectionalToLight(uint d)
            {
                return -normalize(d == 0u ? giFrame.sunTravel.xyz : giFrame.light1Travel.xyz);
            }
            vec3 PathTracingDirectionalRadiance(uint d)
            {
                return d == 0u ? giFrame.sunRadiance.rgb : giFrame.light1Radiance.rgb;
            }
            float PathTracingDirectionalSinHalfAngle(uint d)
            {
                return d == 0u ? SunParams.x : SunParams.z;
            }
            // Light 1 with CastShadows off is never occluded (UE: a light without PATHTRACER_FLAG_CAST_SHADOW_MASK
            // traces no visibility ray), as the deferred pass shades it; the sun's light ray is always traced.
            bool PathTracingDirectionalCastsShadows(uint d)
            {
                return d == 0u || giFrame.light1Radiance.w > 0.5;
            }
            // The cloud shadow map is projected along the sun: only the sun's light is weighed by it (the deferred
            // pass shades light 1 without it as well).
            float PathTracingDirectionalClouds(uint d, vec3 origin)
            {
                return d == 0u ? CloudShadowFactor(origin) : 1.0;
            }
            // A sky with no light (the black cube of a view without an environment, or a black sky) is no light
            // source: its mean weight is 0, and every pdf of it would divide by that mean.
            bool PathTracingHasSky()
            {
                return SkyParams.x > 0u && PathTracingSkyMean(SkyParams.x, SkyParams.y) > 0.0;
            }

            // A directional light's cone: 1 - cos(theta max) from sin of its half angle (UE DirectionalLight_TraceLight).
            float PathTracingConeOneMinusCos(float sinHalfAngle)
            {
                const float s2 = sinHalfAngle * sinHalfAngle;
                return s2 < 0.01 ? s2 * (0.5 + 0.125 * s2) : 1.0 - sqrt(1.0 - s2);
            }

            // UE EstimateLight: the light's share in the choice at P.
            float PathTracingEstimate(uint light, vec3 P)
            {
                if (light == 0u || light == 2u)
                {
                    const uint d = light == 0u ? 0u : 1u;
                    return PathTracingHasDirectional(d) ? PathTracingLuminance(PathTracingDirectionalRadiance(d)) : 0.0;
                }
                if (light == 1u)
                    return PathTracingHasSky() ? PathTracingSkyEstimate(SkyParams.x, SkyParams.y) : 0.0;
                const uint i = light - kPathTracingFirstLocalLight;
                if (i < Counts.x)
                    return PathTracingLuminance(DesertPointLightAt(pointLights[i], P).Radiance);
                return PathTracingLuminance(DesertSpotLightAt(spotLights[i - Counts.x], P).Radiance);
            }

            float PathTracingEstimateTotal(vec3 P)
            {
                float total = 0.0;
                const uint lights = kPathTracingFirstLocalLight + Counts.x + Counts.y;
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
                const uint lights = kPathTracingFirstLocalLight + Counts.x + Counts.y;
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
                if (chosen == 0u || chosen == 2u)
                {
                    const uint  d        = chosen == 0u ? 0u : 1u;
                    const vec3  toLight  = PathTracingDirectionalToLight(d);
                    const float sinHalf  = PathTracingDirectionalSinHalfAngle(d);
                    vec3        L        = toLight;
                    float       lightPdf = 0.0; // 0: a delta direction, no MIS
                    if (sinHalf > 0.0)
                    {
                        // UE UniformSampleConeRobust over the source angle.
                        const float oneMinusCos = PathTracingConeOneMinusCos(sinHalf);
                        const float cosTheta    = 1.0 - u.x * oneMinusCos;
                        const float sinTheta    = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
                        const float phi         = 2.0 * PI * u.y;
                        L        = PathTracingBasis(toLight) * vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
                        lightPdf = 1.0 / (2.0 * PI * oneMinusCos);
                    }
                    if (dot(N, L) <= 0.0)
                        return vec3(0.0);
                    const vec3 transmittance =
                        PathTracingDirectionalCastsShadows(d) ? PathTracingTransmittance(origin, L, 1.0e9) : vec3(1.0);
                    if (all(lessThanEqual(transmittance, vec3(0.0))))
                        return vec3(0.0);
                    // The sun's light crosses the cloud layer on its way down: its transmittance from this vertex
                    // (the same factor the sun disc a material ray from here meets is weighed by, PathTracingMissRadiance).
                    const float clouds = PathTracingDirectionalClouds(d, origin);
                    // radiance / pdf of the cone sample is the light's colour (the cone's radiance is normalised by
                    // its solid angle, UE DirectionalLight_SampleLight).
                    const float weight = lightPdf > 0.0
                                             ? PathTracingMISWeightPower(pickPdf * lightPdf, PathTracingBsdfPdf(m, N, V, L))
                                             : 1.0;
                    return PathTracingBsdfCos(m, N, V, L) * PathTracingDirectionalRadiance(d) * transmittance *
                           (clouds * weight / pickPdf);
                }
                if (chosen == 1u)
                {
                    const PathTracingSkySample s = PathTracingSkySampleLight(u, SkyParams.x, SkyParams.y);
                    if (s.Pdf <= 0.0 || dot(N, s.Direction) <= 0.0)
                        return vec3(0.0);
                    const vec3 transmittance = PathTracingTransmittance(origin, s.Direction, 1.0e9);
                    if (all(lessThanEqual(transmittance, vec3(0.0))))
                        return vec3(0.0);
                    const float weight = PathTracingMISWeightPower(pickPdf * s.Pdf, PathTracingBsdfPdf(m, N, V, s.Direction));
                    return PathTracingBsdfCos(m, N, V, s.Direction) * s.Radiance * transmittance * weight /
                           (pickPdf * s.Pdf);
                }
                const uint  i = chosen - kPathTracingFirstLocalLight;
                DesertLight light;
                vec3        position;
                float       sourceRadius;
                if (i < Counts.x)
                {
                    light        = DesertPointLightAt(pointLights[i], P);
                    position     = pointLights[i].position;
                    sourceRadius = pointLights[i].sourceRadius;
                }
                else
                {
                    light        = DesertSpotLightAt(spotLights[i - Counts.x], P);
                    position     = spotLights[i - Counts.x].position;
                    sourceRadius = spotLights[i - Counts.x].sourceRadius;
                }
                float      reach;
                const vec3 toLight = SampleLightSourceSphere(normalize(position - origin), distance(position, origin),
                                                             sourceRadius, u, reach);
                if (dot(N, toLight) <= 0.0)
                    return vec3(0.0);
                const vec3 transmittance = PathTracingTransmittance(origin, toLight, reach);
                return PathTracingBsdfCos(m, N, V, toLight) * light.Radiance * transmittance / pickPdf;
            }

            // ---- The emissive triangles. ----
            bool PathTracingHasEmissive()
            {
                return EmissiveLightsTriangleCount(EmissiveCounts, EmissiveDynamicCounts) > 0u &&
                       EmissiveLightsTotal(EmissiveCounts, EmissiveDynamicCounts) > 0.0;
            }

            // The list's row of the triangle the last material ray met (kEmissiveNone: not in the list).
            uint PathTracingEmissiveTriangleOfHit()
            {
                return EmissiveLightsTriangleOfHit(payload.InstanceIndex, payload.GeometryIndex, payload.PrimitiveIndex,
                                                   EmissiveCounts);
            }

            // Next event estimation over the emissive triangles from @p origin (the vertex's offset origin, @p coneWidth
            // its ray cone): a triangle by power, a point by area, its solid-angle density weighed against the material's.
            vec3 PathTracingSampleEmissive(PathTracingMaterial m, vec3 N, vec3 V, vec3 origin, float coneWidth,
                                           inout uint rng)
            {
                const float pick = PathTracingRandom(rng);
                const vec2  u    = PathTracingRandom2(rng);
                if (!PathTracingHasEmissive())
                    return vec3(0.0);
                const uint triangle = EmissiveLightsPick(pick, EmissiveCounts, EmissiveDynamicCounts);
                if (triangle >= EmissiveLightsTriangleCount(EmissiveCounts, EmissiveDynamicCounts))
                    return vec3(0.0);
                const EmissiveTriangle t       = EmissiveLightsTriangle(triangle, EmissiveCounts);
                const vec3             toPoint = EmissiveLightsSamplePoint(t, u) - origin;
                const float            dist    = length(toPoint);
                if (dist <= 0.0)
                    return vec3(0.0);
                const vec3 L = toPoint / dist;
                if (dot(N, L) <= 0.0)
                    return vec3(0.0);
                const float lightPdf = EmissiveLightsSolidAnglePdf(
                    t, EmissiveLightsPickPdf(triangle, EmissiveCounts, EmissiveDynamicCounts), dist, dot(EmissiveLightsNormal(t), L));
                if (lightPdf <= 0.0)
                    return vec3(0.0);
                // Past the point by a margin, so the closest hit can land on the sampled triangle itself; anything
                // else it meets first occludes it.
                PathTracingTraceMaterialTo(origin, L, coneWidth, dist * 1.001 + 0.1);
                if (payload.HitT < 0.0 || PathTracingEmissiveTriangleOfHit() != triangle)
                    return vec3(0.0);
                const float weight = PathTracingMISWeightPower(lightPdf, PathTracingBsdfPdf(m, N, V, L));
                return PathTracingBsdfCos(m, N, V, L) * payload.Emissive * weight / lightPdf;
            }

            // The weight of emission a material ray met (drawn with density @p bsdfPdf along @p dir; < 0: the camera
            // ray): the power heuristic against the emissive sampler's density of the same point from the vertex the
            // ray left. A surface not in the list cannot be sampled as a light, so its emission takes the whole weight.
            float PathTracingEmissiveHitWeight(vec3 dir, float bsdfPdf)
            {
                if (bsdfPdf < 0.0 || !PathTracingHasEmissive())
                    return 1.0;
                const uint triangle = PathTracingEmissiveTriangleOfHit();
                if (triangle >= EmissiveLightsTriangleCount(EmissiveCounts, EmissiveDynamicCounts))
                    return 1.0;
                const EmissiveTriangle t        = EmissiveLightsTriangle(triangle, EmissiveCounts);
                const float            lightPdf = EmissiveLightsSolidAnglePdf(
                    t, EmissiveLightsPickPdf(triangle, EmissiveCounts, EmissiveDynamicCounts), payload.HitT,
                    dot(EmissiveLightsNormal(t), dir));
                return PathTracingMISWeightPower(bsdfPdf, lightPdf);
            }

            // What a material ray that left the scene sees: the sky, and each directional light's disc when it has a
            // size. Weighted against the light sampler of the vertex it left (lightPickSky / lightPickDirectional: that
            // vertex's choice probabilities, x the sun, y light 1; bsdfPdf < 0: a camera ray, nothing to weigh). The sky carries the clouds (the sky capture
            // marches them into the environment); the disc is seen through them from @p origin, the ray's start.
            vec3 PathTracingMissRadiance(vec3 origin, vec3 direction, float bsdfPdf, float lightPickSky, vec2 lightPickDirectional)
            {
                vec3 radiance = vec3(0.0);
                if (bsdfPdf < 0.0)
                    radiance += ApplySkyGain(textureLod(EnvironmentSpecularTexture,
                                                        SkyLookDirection(direction, giFrame.skyYawCosSin.xy), 0.0).rgb,
                                             giFrame.skyGain.rgb);
                else if (PathTracingHasSky())
                {
                    const vec4 sky = PathTracingSkyEval(direction, SkyParams.x, SkyParams.y);
                    radiance += sky.rgb * PathTracingMISWeightPower(bsdfPdf, lightPickSky * sky.w);
                }
                for (uint d = 0u; d < 2u; ++d)
                {
                    const float sinHalf = PathTracingDirectionalSinHalfAngle(d);
                    if (!PathTracingHasDirectional(d) || sinHalf <= 0.0)
                        continue;
                    const float oneMinusCos = PathTracingConeOneMinusCos(sinHalf);
                    if (1.0 - dot(direction, PathTracingDirectionalToLight(d)) < oneMinusCos)
                    {
                        const float solidAngle = 2.0 * PI * oneMinusCos;
                        const float pick       = d == 0u ? lightPickDirectional.x : lightPickDirectional.y;
                        const float weight =
                            bsdfPdf < 0.0 ? 1.0 : PathTracingMISWeightPower(bsdfPdf, pick / solidAngle);
                        radiance += PathTracingDirectionalRadiance(d) *
                                    (PathTracingDirectionalClouds(d, origin) / solidAngle * weight);
                    }
                }
                return radiance;
            }

            // UE PathTracingCore.ush AccumulateRadiance (r.PathTracing.MaxPathIntensity): one contribution of a path,
            // scaled down with its hue kept so that its largest channel is at most SunParams.w, unless @p cameraRay
            // (what the camera ray itself sees is never clamped). Bounds the pixel's variance - the fireflies - at the
            // price of a bias toward darker highlights.
            void PathTracingAccumulate(inout vec3 radiance, vec3 contribution, bool cameraRay)
            {
                const float brightest = max(contribution.r, max(contribution.g, contribution.b));
                if (!cameraRay && SunParams.w > 0.0 && brightest > SunParams.w)
                    contribution *= SunParams.w / brightest;
                radiance += contribution;
            }

            // One path of the pixel: the camera ray, then up to Counts.w bounces. @p sampleIndex is the path's index
            // in the pixel's accumulation (paths summed before it + its place in this frame), so every path of the
            // pixel since the restart draws its own random sequence.
            vec3 PathTracingTracePath(ivec2 pixel, ivec2 size, uint sampleIndex)
            {
                uint rng = PathTracingHash(uint(pixel.x) + PathTracingHash(uint(pixel.y) + PathTracingHash(sampleIndex + PathTracingHash(SkyParams.z))));

                // The camera ray through a uniformly chosen point of the pixel (a box filter).
                const vec2 uv     = (vec2(pixel) + PathTracingRandom2(rng)) / vec2(size);
                vec3       origin = cameraUB.CameraPos;
                vec3       dir    = normalize(ReconstructWorldPosition(uv, 0.5, cameraUB.InvJitteredViewProjection) - origin);

                vec3  radiance     = vec3(0.0);
                vec3  throughput   = vec3(1.0);
                float bsdfPdf      = -1.0; // the density the current ray was drawn with; < 0: the camera ray
                float lightPickSky = 0.0;
                vec2  lightPickDirectional = vec2(0.0);
                float coneWidth    = 0.0;
                // Glass on the path: the absorption coefficient of the solid the ray is inside (0: outside), and
                // whether a glass interface lies between the ray and the last vertex that sampled the lights - the
                // emissive light samples stop at glass, so emission met past it takes the whole weight.
                vec3 absorption = vec3(0.0);
                bool pastGlass  = false;
                // The ray is still the camera's: no material vertex scattered it yet (glass interfaces are delta
                // lobes and keep it), so what it meets is not clamped (PathTracingAccumulate).
                bool cameraRay  = true;
                for (uint bounce = 0u;; ++bounce)
                {
                    PathTracingTraceMaterial(origin, dir, coneWidth);
                    if (payload.HitT < 0.0)
                    {
                        PathTracingAccumulate(radiance,
                                              throughput * PathTracingMissRadiance(origin, dir, bsdfPdf, lightPickSky,
                                                                                   lightPickDirectional),
                                              cameraRay);
                        break;
                    }
                    throughput *= exp(-absorption * payload.HitT);
                    const vec3 P = origin + dir * payload.HitT;
                    const vec3 N = payload.Normal;
                    const vec3 V = -dir;
                    // Emission the material ray met, weighed against the emissive light sampler.
                    if (any(greaterThan(payload.Emissive, vec3(0.0))))
                        PathTracingAccumulate(radiance,
                                              throughput * payload.Emissive *
                                                  PathTracingEmissiveHitWeight(dir, pastGlass ? -1.0 : bsdfPdf),
                                              cameraRay);

                    coneWidth += SunParams.y * payload.HitT;
                    // The next rays leave the surface by an offset that grows with the distance travelled, so they do
                    // not meet the triangle they start on.
                    const float offset = 0.05 + 1.0e-5 * length(P - cameraUB.CameraPos);
                    if (PathTracingIsTransmissive())
                    {
                        // A glass interface: reflect with the Fresnel probability, otherwise transmit; each choice's
                        // weight is its probability, so the throughput keeps only the tint and the absorption.
                        if (bounce >= Counts.w)
                            break;
                        const bool  front = (payload.Flags & kDesertRayPayloadFrontFace) != 0u;
                        const bool  thin  = payload.Ior <= 1.0;
                        const float eta   = front ? 1.0 / payload.Ior : payload.Ior;
                        const float F     = thin ? 0.0 : PathTracingFresnelDielectric(max(dot(N, V), 0.0), eta);
                        pastGlass         = true;
                        if (PathTracingRandom(rng) < F)
                        {
                            // A mirror direction no light sample reaches: what it meets takes the whole weight.
                            dir     = reflect(dir, N);
                            origin  = P + N * offset;
                            bsdfPdf = -1.0;
                        }
                        else if (thin)
                        {
                            // Straight through: the direction, and so the last vertex's MIS state, is unchanged.
                            throughput *= payload.Transmission;
                            origin = P - N * offset;
                        }
                        else
                        {
                            dir        = refract(dir, N, eta);
                            origin     = P - N * offset;
                            absorption = front ? PathTracingAbsorption(payload.Transmission) : vec3(0.0);
                        }
                        continue;
                    }

                    const PathTracingMaterial m = PathTracingMaterialOf(N, V);
                    const vec3  next  = P + N * offset;
                    const float total = PathTracingEstimateTotal(P);
                    PathTracingAccumulate(radiance, throughput * PathTracingSampleLights(m, P, N, V, next, total, rng),
                                          false);
                    PathTracingAccumulate(radiance,
                                          throughput * PathTracingSampleEmissive(m, N, V, next, coneWidth, rng),
                                          false);
                    if (bounce >= Counts.w)
                        break;
                    cameraRay = false;

                    lightPickSky = total > 0.0 ? PathTracingEstimate(1u, P) / total : 0.0;
                    lightPickDirectional = total > 0.0 ? vec2(PathTracingEstimate(0u, P), PathTracingEstimate(2u, P)) / total
                                                       : vec2(0.0);
                    const vec3 L = PathTracingSampleBsdf(m, N, V, rng);
                    bsdfPdf      = PathTracingBsdfPdf(m, N, V, L);
                    pastGlass    = false;
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

            // This frame's SkyParams.w paths of the pixel (PathTracing.PathsPerFrame, cut to what the total
            // PathTracing.SamplesPerPixel leaves), added to the running sum in one read and one write.
            void main()
            {
                const ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
                const ivec2 size  = imageSize(AccumulationTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                vec3 sum = vec3(0.0);
                for (uint s = 0u; s < SkyParams.w; ++s)
                    sum += PathTracingTracePath(pixel, size, Counts.z + s);
                const vec4 previous = Counts.z == 0u ? vec4(0.0) : imageLoad(AccumulationTexture, pixel);
                imageStore(AccumulationTexture, pixel, vec4(previous.rgb + sum, previous.a + float(SkyParams.w)));
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

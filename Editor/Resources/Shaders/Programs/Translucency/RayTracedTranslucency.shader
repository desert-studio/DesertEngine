Shader "RayTracedTranslucency"
{
    // RAY-TRACED REFRACTION (Translucency.Refraction HardwareTraced; UE RayTracedTranslucency, MaxPrimaryHitEvents;
    // System::RayTracedTranslucencyRenderer, the pass "Translucency: Ray Traced Refraction"). One ray per pixel of the
    // translucent front layer (Mesh/Surface/Pass_FrontLayer.glslh: rgb = world normal, a = roughness; zero = no
    // translucent surface): it starts just before the front layer's depth on the camera ray, so its first hit is that
    // glass, whose closest hit (Mesh/Surface/Pass_RayHit.glslh) gives Transmission, Ior and the shading normal. Every
    // dielectric interface (Transmission > 0) bends the ray by Snell - entering or leaving by the payload's front face
    // - or mirrors it past the critical angle (total internal reflection); a thin sheet (Ior 1) tints per crossing,
    // solid glass absorbs by Beer's law between its faces (PathTracer.shader, the same statement), and the smooth
    // Fresnel transmission of every interface after the first multiplies the path (the glass cell weighs its own
    // first interface by its Fresnel). The first opaque hit is lit in place as the hit-lit reflections light theirs
    // (Common/GIHitLighting.glslh GIHitRadianceLit, its emission included); a miss is the sky. A rough front layer
    // bends by one GGX visible normal per frame (the temporal upscaler sums them). Output: rgb = pre-exposed radiance
    // x transmittance, a = 1 where the trace answered; past MaxRoughness a = 0 (the glass's screen tier answers), and
    // past MaxInterfaces the scene copy is read where the path's last interface projects on screen.
    Pass "Refraction"
    {
        RayGen
        {
            Uniform sampler LinearClampSampler;
            Uniform sampler PointClampSampler;
            Uniform(1) texture2D FrontLayerNormalTexture; // rgb = world normal, a = roughness (cleared: zero)
            #define FrontLayerNormalTexture sampler2D(FrontLayerNormalTexture, PointClampSampler)
            Uniform(2) texture2D GBufferDepthTexture;     // the opaque scene's device depth (GIHitLighting's screen material)
            #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
            Uniform(3) texture2D SceneCopyTexture;        // the scene copy before the glass (RayTracedTranslucencyInputs::SceneCopy)
            #define SceneCopyTexture sampler2D(SceneCopyTexture, LinearClampSampler)
            layout(binding = 4, rgba16f) writeonly uniform image2D TranslucencyRefractionTexture; // System::kTranslucencyRefractionTexture
            Uniform(5) texture2D FrontLayerDepthTexture;  // the front layer's device depth (cleared: far)
            #define FrontLayerDepthTexture sampler2D(FrontLayerDepthTexture, PointClampSampler)

            #define DESERT_GI_TRACER_HARDWARE
            #define DESERT_GI_HIT_LIGHTING_IN_PLACE
            #include <Common/CameraUB.glslh> // the view block the hit groups read
            #include <Common/RayTracingSceneReads.glslh> // the world's table and rain occlusion the hits' weather reads
            #include <Common/GIHitLitReads.glslh>
            #include <Common/RayTracingPayload.glslh>
            #include <Common/ReconstructPosition.glslh>
            #include <Common/MonteCarlo.glslh>

            layout(location = 0) rayPayloadEXT DesertRayPayload payload;

            // System::RayTracedTranslucencyParams, member for member (std430 push constants).
            PushConstant RayTracedTranslucencyParams
            {
                mat4  InvJitteredViewProjection;
                vec4  CameraPosition; // xyz world (cm), w = PreExposure
                float MaxRayDistance; // cm, each segment
                float MaxRoughness;   // above it the surface is not traced (a = 0)
                uint  MaxInterfaces;  // dielectric interfaces followed before the path stops
                uint  FrameIndex;     // the rough lobe's sequence
            };

            // Solid glass reaches its tint after this many centimetres inside it (Beer's law): PathTracer.shader's
            // kPathTracingAbsorptionDistance, so the traced raster and the reference absorb alike.
            const float kRefractionAbsorptionDistance = 100.0;
            // How far before the front layer's depth the ray starts (cm), so the glass is its first hit.
            const float kRefractionStartBackoff = 1.0;

            // UE FresnelDielectric (PathTracer.shader PathTracingFresnelDielectric): the reflectance of a smooth
            // dielectric for the incident cosine cosI and eta = n(incident) / n(transmitted); 1 past the critical angle.
            float RefractionFresnelDielectric(float cosI, float eta)
            {
                const float sin2T = eta * eta * (1.0 - cosI * cosI);
                if (sin2T >= 1.0)
                    return 1.0;
                const float cosT = sqrt(1.0 - sin2T);
                const float rs   = (eta * cosI - cosT) / (eta * cosI + cosT);
                const float rp   = (cosI - eta * cosT) / (cosI + eta * cosT);
                return 0.5 * (rs * rs + rp * rp);
            }

            // The material ray of the hit groups: every instance (translucent ones included), one record per segment.
            void RefractionTrace(vec3 origin, vec3 direction, float coneWidth, float spread)
            {
                payload.HitT         = -1.0;
                payload.ConeWidth    = coneWidth;
                payload.ConeSpread   = spread;
                payload.Transmission = vec3(0.0);
                traceRayEXT(SceneTlas, gl_RayFlagsNoneEXT, 0xFFu, 0u, 1u, 0u, origin, 0.0, direction, MaxRayDistance, 0);
            }

            // A GGX visible normal around @p N for the view direction @p V (both world), keyed on the pixel and frame.
            vec3 RefractionRoughNormal(vec3 N, vec3 V, float roughness, ivec2 pixel)
            {
                const vec3  up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
                const vec3  T  = normalize(cross(up, N));
                const vec3  B  = cross(N, T);
                const vec3  ve = vec3(dot(V, T), dot(V, B), max(dot(V, N), 1.0e-4));
                const vec2  u  = vec2(hash12(vec2(pixel) + float(FrameIndex) * 17.31),
                                      hash12(vec2(pixel.yx) + float(FrameIndex) * 7.13 + 0.5));
                const vec3  h  = SampleGGXVNDF(ve, roughness * roughness, u);
                return normalize(T * h.x + B * h.y + N * h.z);
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
                const ivec2 size  = imageSize(TranslucencyRefractionTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const vec4 front = texelFetch(FrontLayerNormalTexture, pixel, 0);
                if (dot(front.xyz, front.xyz) <= 0.0 || front.a > MaxRoughness)
                {
                    imageStore(TranslucencyRefractionTexture, pixel, vec4(0.0));
                    return;
                }
                const vec2  uv          = (vec2(pixel) + 0.5) / vec2(size);
                const float frontDevice = texelFetch(FrontLayerDepthTexture, pixel, 0).r;
                const vec3  frontPos    = ReconstructWorldPosition(uv, frontDevice, InvJitteredViewProjection);
                const vec3  eye         = CameraPosition.xyz;
                vec3        dir         = normalize(frontPos - eye);
                vec3        origin      = frontPos - dir * kRefractionStartBackoff;
                const float spread      = 2.0 / (abs(cameraUB.Projection[1][1]) * float(size.y));
                float       coneWidth   = spread * distance(eye, frontPos);

                vec3 transmittance = vec3(1.0);
                vec3 absorption    = vec3(0.0); // 1/cm inside the solid the path is in
                vec3 lastPoint     = frontPos;
                for (uint crossing = 0u; crossing < MaxInterfaces; ++crossing)
                {
                    RefractionTrace(origin, dir, coneWidth, spread);
                    if (payload.HitT < 0.0)
                    {
                        imageStore(TranslucencyRefractionTexture, pixel,
                                   vec4(GIMissRadiance(dir) * transmittance * CameraPosition.w, 1.0));
                        return;
                    }
                    const vec3 P = origin + dir * payload.HitT;
                    transmittance *= exp(-absorption * payload.HitT);
                    coneWidth += spread * payload.HitT;
                    if (all(lessThanEqual(payload.Transmission, vec3(0.0))))
                    {
                        // The first opaque surface: lit in place, as the hit-lit reflections light theirs.
                        GIHit hit;
                        hit.position     = P;
                        hit.distance     = payload.HitT;
                        hit.normal       = payload.Normal;
                        hit.meshCards    = GI_NO_MESH_CARDS;
                        hit.instance     = GI_NO_INSTANCE;
                        hit.primitive    = 0u;
                        hit.barycentrics = vec2(0.0);
                        hit.frontFace    = (payload.Flags & kDesertRayPayloadFrontFace) != 0u;
                        hit.velocity     = vec3(0.0);
                        hit.motionKnown  = false;
                        hit.globalField  = false;
                        const vec3 lit = GIHitRadianceLit(hit, dir, GIHitSurface(payload.BaseColor, payload.Metallic,
                                                                                 payload.Roughness, payload.Emissive,
                                                                                 true, payload.ShadingWord));
                        imageStore(TranslucencyRefractionTexture, pixel,
                                   vec4(lit * transmittance * CameraPosition.w, 1.0));
                        return;
                    }
                    // A dielectric interface. The payload's normal faces the ray.
                    const bool  frontFace = (payload.Flags & kDesertRayPayloadFrontFace) != 0u;
                    const bool  thin      = payload.Ior <= 1.0;
                    const float eta       = frontFace ? 1.0 / payload.Ior : payload.Ior;
                    vec3        N         = payload.Normal;
                    if (crossing == 0u && front.a > 0.0)
                        N = RefractionRoughNormal(N, -dir, front.a, pixel);
                    const float offset = 0.05 + 1.0e-5 * distance(P, eye);
                    lastPoint          = P;
                    if (thin)
                    {
                        transmittance *= payload.Transmission;
                        origin = P - payload.Normal * offset;
                        continue;
                    }
                    const float cosI = clamp(dot(N, -dir), 0.0, 1.0);
                    const vec3  bent = refract(dir, N, eta);
                    if (dot(bent, bent) <= 0.0)
                    {
                        // Total internal reflection: the mirror bounce stays inside the solid.
                        dir    = reflect(dir, N);
                        origin = P + payload.Normal * offset;
                        continue;
                    }
                    if (crossing > 0u)
                        transmittance *= 1.0 - RefractionFresnelDielectric(cosI, eta);
                    absorption = frontFace ? -log(clamp(payload.Transmission, vec3(1.0e-4), vec3(1.0))) /
                                                 kRefractionAbsorptionDistance
                                           : vec3(0.0);
                    dir    = normalize(bent);
                    origin = P - payload.Normal * offset;
                }
                // The budget of interfaces is spent: the scene copy where the last interface projects on screen.
                const vec4 clip = cameraUB.JitteredViewProjection * vec4(lastPoint, 1.0);
                vec4 result = vec4(0.0);
                if (clip.w > 0.0)
                {
                    const vec2 ndc   = clip.xy / clip.w;
                    const vec2 exitUV = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (all(greaterThanEqual(exitUV, vec2(0.0))) && all(lessThan(exitUV, vec2(1.0))))
                        result = vec4(textureLod(SceneCopyTexture, exitUV, 0.0).rgb * transmittance, 1.0);
                }
                imageStore(TranslucencyRefractionTexture, pixel, result);
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

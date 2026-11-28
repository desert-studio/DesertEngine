// DesertAsset {"Kind":"Shader","Guid":"7ac1660e38214ace8f30d756072b5951","Versions":{"SHDR":1},"Dependencies":[]}
Shader "RayTracedTranslucency"
{
    // RAY-TRACED REFRACTION (Translucency.Refraction HardwareTraced; UE RayTracedTranslucency, MaxPrimaryHitEvents;
    // System::RayTracedTranslucencyRenderer, the pass "Translucency: Ray Traced Refraction"). HALF resolution: one ray
    // per 2x2 quad of the translucent front layer (Mesh/Surface/Pass_FrontLayer.glslh: rgb = world normal, a =
    // roughness; zero = no translucent surface). The quad traces its pixel FrameIndex & 3 (index = x + 2 * y inside the
    // quad), or its first pixel that holds a traceable front layer when that one does not; a quad without one writes
    // zero. The ray starts just before that pixel's front layer depth on the camera ray, so its first hit is that
    // glass, whose closest hit (Mesh/Surface/Pass_RayHit.glslh) gives Transmission, Ior and the shading normal. The
    // path goes through EVERY translucent layer to the first opaque hit or a miss (UE: the refraction ray crosses all
    // translucent layers). The first MaxInterfaces dielectric interfaces (Transmission > 0) BEND the ray by Snell -
    // entering or leaving by the payload's front face - or mirror it past the critical angle (total internal
    // reflection), and the smooth Fresnel transmission of every bending interface after the first multiplies the path
    // (the glass cell weighs its own first interface by its Fresnel); every later one is crossed STRAIGHT (direction
    // kept, no Fresnel). Either way a thin sheet (Ior 1) tints per crossing and solid glass absorbs by Beer's law
    // between its faces (PathTracer.shader, the same statement). Every layer behind the first adds its own emission,
    // weighed by the transmittance in front of it. kRefractionMaxSegments (8) bounds the segments; a path that reaches
    // it answers with the emission it gathered. The first opaque hit is lit SCREEN FIRST: a hit the view sees
    // (projected by giFrame.viewProjection into the frame, the opaque depth there within GIRayBias().screenTolerance of
    // it - the test of Common/GIHitLighting.glslh GIHitMaterial) takes this frame's lit scene colour from the scene
    // copy; a hit the view does not see is lit IN PLACE on every tier, whatever Reflections.HitLighting is
    // (GIHitRadianceLit with the payload's material, its emission included; UE RT Translucency lights its hits in
    // place and reads no surface cache for the primary visible path - the cards' black back of one-sided geometry
    // stays a rule of the GI rays only) and leaves a mark request for the radiance cache cells its bounce reads
    // (RadianceCacheMarkRequests.glslh); a miss is the sky. A rough front layer bends by one GGX visible normal per
    // frame (the temporal upscaler sums them). Output (one texel per quad):
    // rgb = the layers' pre-exposed emission + the end's pre-exposed radiance x transmittance; a = 1 + the traced
    // pixel's index in the quad - every started trace answers. a = 0 only where no trace starts (a quad without a front
    // layer, or rougher than MaxRoughness) and under Composite 0 (no ray is traced) - the glass cell
    // (Mesh/Surface/TranslucencyRefraction.glslh) reads the scene copy itself there.
    Pass "Trace"
    {
        RayGen
        {
            // THE SLOTS (System::RayTracedTranslucencyShaderParameters + SSRFrontLayerShaderParameters +
            // SSRShaderParameters as SSRRenderer::DeclareTracedReads binds them; numbers are this program's).
            Uniform sampler LinearClampSampler;
            Uniform sampler PointClampSampler;
            Uniform(1) texture2D FrontLayerNormalTexture; // rgb = world normal, a = roughness (cleared: zero)
            #define FrontLayerNormalTexture sampler2D(FrontLayerNormalTexture, PointClampSampler)
            Uniform(2) texture2D GBufferDepthTexture;     // the opaque scene's device depth (GIHitLighting's screen material)
            #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
            layout(binding = 4, rgba16f) writeonly uniform image2D RefractionOutputTexture; // System::RayTracedTranslucencyShaderParameters; the glass cell binds it as kTranslucencyRefractionTexture
            Uniform(5) texture2D FrontLayerDepthTexture;  // the front layer's device depth (cleared: far)
            #define FrontLayerDepthTexture sampler2D(FrontLayerDepthTexture, PointClampSampler)
            Uniform texture2D SceneColorCopyTexture;      // this frame's opaque lit scene, pre-exposed: a seen hit's radiance
            #define SceneColorCopyTexture sampler2D(SceneColorCopyTexture, LinearClampSampler)

            // The program exists only as the hardware tracer lit in place, on every tier (RayTracedTranslucencyRenderer::
            // Variant = TracerVariant(RayTracedTranslucencyTracer), whose Hits is InPlace, defines both). The boot
            // compile has no variant defines and is that same program, so it registers the pass the renderer acquires
            // its variant from. In place, GIHitLighting.glslh includes no surface cache (SurfaceCacheSampling.glslh),
            // so this program declares none of its bindings and needs no GI_SURFACE_CACHE_BINDING.
            #ifndef DESERT_GI_TRACER_HARDWARE
            #define DESERT_GI_TRACER_HARDWARE
            #endif
            #ifndef DESERT_GI_HIT_LIGHTING_IN_PLACE
            #define DESERT_GI_HIT_LIGHTING_IN_PLACE
            #endif
            #include <Common/CameraUB.glslh> // the view block the hit groups read
            #include <Common/RayTracingSceneReads.glslh> // the world's table and rain occlusion the hits' weather reads
            #include <Common/GIHitLitReads.glslh>
            #include <Common/GIHitLighting.glslh>
            #include <Common/RayTracingPayload.glslh>
            #include <Common/ReconstructPosition.glslh>
            #include <Common/MonteCarlo.glslh>
            #include <Common/RayTracedTranslucencyPush.glslh> // InvJitteredViewProjection ... Composite
            // The cache's request table (kMarkRequestsBinding): a path end lit in place reads the cache where no pixel
            // marked; 49 is past every slot above (the tracer's 23..33, the scene reads 35..37, the atlas 41).
            #define RADIANCE_CACHE_MARK_REQUESTS_BINDING 49
            #include <Common/RadianceCacheMarkRequests.glslh>

            layout(location = 0) rayPayloadEXT DesertRayPayload payload;

            // Solid glass reaches its tint after this many centimetres inside it (Beer's law): PathTracer.shader's
            // kPathTracingAbsorptionDistance, so the traced raster and the reference absorb alike.
            const float kRefractionAbsorptionDistance = 100.0;
            // How far before the front layer's depth the ray starts (cm), so the glass is its first hit.
            const float kRefractionStartBackoff = 1.0;
            // The hard bound of traced segments per path; MaxInterfaces bounds only the BENDS. A budget that ended the
            // path threw its answer away: on the Bistro shop window the fifth interface of the tier's 4 is met almost
            // everywhere, so the rays cost the pass and the glass showed the scene copy (MaxInterfaces 1 vs the tier
            // 5.09, Composite 0 vs 1 5.42, both under the run's own noise 7.43; MaxInterfaces 8 13.81). 8 is a COST
            // bound against the pass's 1.0 ms at 1080p: at 16 the pass cost 1.391 ms by day and 1.443 ms by night at
            // 1.17 Mpx (2.46 / 2.55 ms at 1080p), nearly every path reaching an opaque end lit in place. 8 is nearer
            // UE's bound (r.RayTracing.Translucency.MaxRefractionRays 3): 8 thin sheets of 0.9 pass 0.9^8 = 0.43,
            // 8 clear solids crossed straight pass nearly all, and the light of an end past the bound is dropped.
            const uint kRefractionMaxSegments = 8u;

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

            // Beer's law inside solid glass (1/cm): entering by the front face starts the absorption that reaches the
            // payload's Transmission after kRefractionAbsorptionDistance, leaving ends it.
            vec3 RefractionAbsorption(vec3 transmission, bool frontFace)
            {
                return frontFace ? -log(clamp(transmission, vec3(1.0e-4), vec3(1.0))) / kRefractionAbsorptionDistance
                                 : vec3(0.0);
            }

            // The material ray of the hit groups: every instance (translucent ones included), one record per segment.
            void RefractionTrace(vec3 origin, vec3 direction, float coneWidth, float spread)
            {
                payload.HitT         = -1.0;
                payload.ConeWidth    = coneWidth;
                payload.ConeSpread   = spread;
                payload.Transmission = vec3(0.0);
#if defined( DESERT_SER ) && DESERT_SER
                // SHADER EXECUTION REORDERING (RayTracedTranslucencyRenderer::Variant(reorder)): regroup by the hit's
                // SBT record before its closest hit, as SSRTrace.glslh TraceWorld; the result is the plain trace's.
                hitObjectNV serHit;
                hitObjectTraceRayNV(serHit, SceneTlas, gl_RayFlagsNoneEXT, 0xFFu, 0u, 1u, 0u, origin, 0.0, direction,
                                    MaxRayDistance, 0);
                reorderThreadNV(serHit, hitObjectIsHitNV(serHit) ? (hitObjectGetShaderBindingTableRecordIndexNV(serHit) & 0xFFFFu)
                                                                 : 0xFFFFu,
                                16u);
                hitObjectExecuteShaderNV(serHit, 0);
#else
                traceRayEXT(SceneTlas, gl_RayFlagsNoneEXT, 0xFFu, 0u, 1u, 0u, origin, 0.0, direction, MaxRayDistance, 0);
#endif
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

            // The pre-exposed radiance of the first opaque hit at @p P (the payload's hit), SCREEN FIRST: the scene copy
            // where the view sees that very point (GIHitMaterial's test: giFrame.viewProjection, the opaque depth within
            // the tracer's tolerance; the copy is already pre-exposed); a hit the view does not see is lit IN PLACE on
            // every tier (GIHitRadianceLit with the payload's material, never GIHitMaterial's G-buffer guess, nor the
            // surface cache's cards: the primary visible path is not GI, so it never reads the cards' black back of
            // one-sided geometry - UE RT Translucency lights its hits in place).
            vec3 RefractionOpaqueRadiance(vec3 P, vec3 dir)
            {
                const vec4 clip = giFrame.viewProjection * vec4(P, 1.0);
                if (clip.w > 0.0)
                {
                    const vec2 ndc = clip.xy / clip.w;
                    if (all(lessThanEqual(abs(ndc), vec2(1.0))))
                    {
                        const vec2  uv    = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                        const float depth = textureLod(GBufferDepthTexture, uv, 0.0).r;
                        const vec3  seen  = ReconstructWorldPosition(uv, depth, giFrame.invViewProjection);
                        if (distance(seen, P) <= GIRayBias(P, payload.Normal).screenTolerance)
                            return textureLod(SceneColorCopyTexture, uv, 0.0).rgb;
                    }
                }
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
                const GIHitSurface surface =
                    GIHitSurface(payload.BaseColor, payload.Metallic, payload.Roughness, payload.Emissive, true,
                                 payload.ShadingWord,
                                 DesertSubsurfaceColor(DesertShadingModelIndex(payload.ShadingWord),
                                                       DesertUnpackPayload(payload.ShadingWord)));
                // GIHitBounce reads the radiance cache here, off screen: ask for its cells (next frame's Mark).
                RadianceCacheRequestMark(P, payload.Normal, -dir);
                const vec3 lit = GIHitRadianceLit(hit, dir, surface);
                return lit * CameraPosition.w;
            }

            // A pixel of the quad the trace may start at: inside the render resolution, a front layer, not too rough.
            bool RefractionTraceable(ivec2 pixel, ivec2 full)
            {
                if (any(greaterThanEqual(pixel, full)))
                    return false;
                const vec4 front = texelFetch(FrontLayerNormalTexture, pixel, 0);
                return dot(front.xyz, front.xyz) > 0.0 && front.a <= MaxRoughness;
            }

            // The pixel @p index (x + 2 * y) of the 2x2 quad @p quad.
            ivec2 RefractionQuadPixel(ivec2 quad, uint index)
            {
                return 2 * quad + ivec2(int(index & 1u), int(index >> 1u));
            }

            void main()
            {
                const ivec2 quad = ivec2(gl_LaunchIDEXT.xy);
                if (any(greaterThanEqual(quad, imageSize(RefractionOutputTexture))))
                    return;
                // Composite 0: no answer and no ray traced; the glass cell reads the scene copy itself.
                if (Composite == 0u)
                {
                    imageStore(RefractionOutputTexture, quad, vec4(0.0));
                    return;
                }
                const ivec2 full = textureSize(FrontLayerNormalTexture, 0);
                // The quad's pixel this frame, or its first traceable one; none: no answer for the quad.
                uint index = FrameIndex & 3u;
                if (!RefractionTraceable(RefractionQuadPixel(quad, index), full))
                {
                    index = 4u;
                    for (uint k = 0u; k < 4u; ++k)
                    {
                        if (RefractionTraceable(RefractionQuadPixel(quad, k), full))
                        {
                            index = k;
                            break;
                        }
                    }
                    if (index == 4u)
                    {
                        imageStore(RefractionOutputTexture, quad, vec4(0.0));
                        return;
                    }
                }
                // a of an answer: 1 + the pixel of the quad the ray started at (the glass cell's code).
                const float answered = 1.0 + float(index);
                const ivec2 pixel    = RefractionQuadPixel(quad, index);
                const float rough    = texelFetch(FrontLayerNormalTexture, pixel, 0).a;

                const vec2  uv          = (vec2(pixel) + 0.5) / vec2(full);
                const float frontDevice = texelFetch(FrontLayerDepthTexture, pixel, 0).r;
                const vec3  frontPos    = ReconstructWorldPosition(uv, frontDevice, InvJitteredViewProjection);
                const vec3  eye         = CameraPosition.xyz;
                vec3        dir         = normalize(frontPos - eye);
                vec3        origin      = frontPos - dir * kRefractionStartBackoff;
                const float spread      = 2.0 / (abs(cameraUB.Projection[1][1]) * float(full.y));
                float       coneWidth   = spread * distance(eye, frontPos);

                vec3 transmittance = vec3(1.0);
                vec3 absorption    = vec3(0.0); // 1/cm inside the solid the path is in
                vec3 radiance      = vec3(0.0); // pre-exposed emission of the layers behind the first
                // One trace per segment, up to kRefractionMaxSegments: the first MaxInterfaces dielectric interfaces
                // bend the ray, every later one is crossed straight; the path ends at an opaque hit or a miss.
                for (uint crossing = 0u; crossing < kRefractionMaxSegments; ++crossing)
                {
                    RefractionTrace(origin, dir, coneWidth, spread);
                    if (payload.HitT < 0.0)
                    {
                        imageStore(RefractionOutputTexture, quad,
                                   vec4(radiance + GIMissRadiance(dir) * transmittance * CameraPosition.w, answered));
                        return;
                    }
                    const vec3 P = origin + dir * payload.HitT;
                    transmittance *= exp(-absorption * payload.HitT);
                    coneWidth += spread * payload.HitT;
                    if (all(lessThanEqual(payload.Transmission, vec3(0.0))))
                    {
                        // The first opaque surface: the scene copy where the view sees it, lit as the tier's
                        // reflections light theirs otherwise (RefractionOpaqueRadiance).
                        imageStore(RefractionOutputTexture, quad,
                                   vec4(radiance + RefractionOpaqueRadiance(P, dir) * transmittance, answered));
                        return;
                    }
                    // A dielectric interface; the payload's normal faces the ray. A layer behind the first adds its
                    // own emission (a lamp's glass, a lit bottle) through what lies in front of it, before its tint;
                    // the first is the glass cell's own surface, which shades its emission itself.
                    if (crossing > 0u)
                        radiance += transmittance * payload.Emissive * CameraPosition.w;
                    const bool  frontFace = (payload.Flags & kDesertRayPayloadFrontFace) != 0u;
                    const float offset    = 0.05 + 1.0e-5 * distance(P, eye);
                    if (payload.Ior <= 1.0)
                    {
                        // A thin sheet tints per crossing and never bends; it ends no solid's absorption it lies in.
                        transmittance *= payload.Transmission;
                        origin = P - payload.Normal * offset;
                        continue;
                    }
                    if (crossing >= MaxInterfaces)
                    {
                        // Past the budget of bends: solid glass is crossed straight, absorbing between its faces as a
                        // bent path does; no Fresnel, as a thin sheet.
                        absorption = RefractionAbsorption(payload.Transmission, frontFace);
                        origin     = P - payload.Normal * offset;
                        continue;
                    }
                    const float eta = frontFace ? 1.0 / payload.Ior : payload.Ior;
                    vec3        N   = payload.Normal;
                    if (crossing == 0u && rough > 0.0)
                        N = RefractionRoughNormal(N, -dir, rough, pixel);
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
                    absorption = RefractionAbsorption(payload.Transmission, frontFace);
                    dir    = normalize(bent);
                    origin = P - payload.Normal * offset;
                }
                // kRefractionMaxSegments interfaces in a row: the answer is the emission they gathered; the light of
                // the path's end past the cost bound is dropped (kRefractionMaxSegments). A value, not a refusal.
                imageStore(RefractionOutputTexture, quad, vec4(radiance, answered));
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

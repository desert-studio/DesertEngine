// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections: tile classification and the half-resolution trace, in ONE compute dispatch.
    // One workgroup per screen tile (grid = ceil(size / 8), tiles stretched over the
    // target). The workgroup first decides whether any pixel of its tile passes the trace's G-buffer gate
    // (Common/SSRGate.glslh) and writes that verdict to the tile mask the denoiser's reconstruction reads;
    // then it traces the half-resolution texels whose source pixel the tile owns - or writes 0 for them when
    // nothing in the tile can reflect. Every trace texel is written every frame, so the target needs no clear,
    // and the classify costs no pass of its own (it used to be a dispatch, and before that a render pass).
    //
    // The trace (SSR2): per traced texel, Reflections.RaysPerPixel rays whose directions are drawn from the GGX
    // distribution of VISIBLE normals (Heitz 2018; UE MonteCarlo.ush ImportanceSampleVisibleGGX), each walked over the
    // closest-depth HZB (Common/HZBTrace.glslh) and validated into a confidence (SSRHitConfidence). The rays are
    // combined with the RATIO ESTIMATOR (Stachowiak 2015, "Stochastic Screen-Space Reflections"; UE SSRT's
    // reflection reconstruction): sum(w * L) / sum(w), w = the BRDF*cos/pdf of the ray without its Fresnel - the
    // split-sum Fresnel-and-geometry term (the BRDF LUT) multiplies the result in the lighting, exactly as it
    // multiplies the reflection environment. Output rgb = the reflected radiance where rays hit, a = the share of
    // the lobe the hits answered (the COVERAGE); 1 - a is answered by the next source - the reflection captures,
    // then the sky (Mesh/AmbientIBL.glslh DesertComposeTracedReflection) - so a miss, the screen edge or a rough
    // lobe hand over smoothly instead of ending.
    //
    // The radiance a hit returns is LAST FRAME's lit scene (UE: the reprojected previous scene colour), read where
    // the hit point was on last frame's screen: the trace runs before this frame's lighting, which then composes it.
    // Compute has no derivatives, so every filtered read is textureLod(..., 0).
    //
    // TRACED (ReflectionMethod::SoftwareTraced, the permutation DESERT_REFLECTIONS_TRACED + the GI tracer's
    // defines, GlobalIllumination::TracerVariant): UE Lumen's reflections (LumenReflectionTracing.cpp
    // TraceReflections: screen, then mesh SDFs, then the global field; LumenReflections.cpp's MaxRoughnessToTrace)
    // over the SAME tracer the GI's screen probes walk - no second tracer. A ray the screen does not answer (it left
    // the screen, went behind the depth buffer's first layer, met the sky or a texel last frame did not see - UE's
    // vignette dither decides at the edge) continues from where the screen was certain: the mesh SDFs of the objects
    // near this tile out to the near field's distance (Common/GITraceMeshSDF.glslh, the tile's own list in shared
    // memory, nearest first, as ScreenProbeTrace.shader per probe), then the global field (Common/GITracer.glslh)
    // to the trace distance - ONE such world ray per texel, drawn among the rays the screen left in proportion to
    // their weight, answering for all of them (UE: one traced ray per texel); a hit returns its surface cache
    // cards' lighting, or is lit in place where no card
    // holds it (GIHitRadiance). A ray that meets nothing is NOT answered: the captures, then the sky, answer it in
    // the composite, as they answer a ScreenSpace miss (UE: the sky light of a Lumen miss; here the captures sit in
    // between). Past kTracedMaxRoughness (UE r.Lumen.Reflections.MaxRoughnessToTrace, faded over
    // kTracedRoughnessFade) a ray only walks the screen, as ScreenSpace does; past kSSRMaxRoughness the captures
    // alone answer. The pass and the denoiser are the ScreenSpace method's.

    Compute
    {
        #include <Common/SSRGate.glslh>
        #include <Common/ReconstructPosition.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest): world position via SSRTraceUB's inverse
        Uniform(3) sampler2D u_SceneColor;      // LAST frame's lit opaque scene (SSRRenderer's scene colour history)
        layout(binding = 4, rgba16f) writeonly uniform image2D u_Trace;    // half resolution
        layout(binding = 5, rgba8) writeonly uniform image2D u_TileMask;   // one texel per tile
        Uniform(7) sampler2D u_HZB;             // closest-depth hierarchy of this frame's depth (HZB.shader)
        Uniform(8) sampler2D u_Velocity;        // NDC current - previous per pixel (View/Velocity.hpp), unjittered
        // The traced texel's ray, for the reconstruction's neighbour reuse (Denoise/SSDReconstructReflections.shader,
        // UE SSRT): xyz = the WORLD point the texel's estimate was reflected from (a neighbour re-aims its own ray at it), w = log2 of
        // the VNDF pdf with which this texel drew that direction. RGBA32F: ViewTargetFormats::kSSRTraceRay.
        layout(binding = 9, rgba32f) writeonly uniform image2D u_TraceRay;  // half resolution, like u_Trace
        #include <Common/HZBTrace.glslh>

        #if defined( DESERT_REFLECTIONS_TRACED )
        // The traced legs' reads (System::SSRRenderer::TracedReads): what GIHitRadiance lights a hit with - the GI
        // frame record, the lights and the world light grid, the radiance cache's read (a neutral one, covering
        // nothing, when the GI does not run it), the sky, the G-buffer's material - then the tracer (23..28), the
        // surface cache (29..35) and the mesh SDF near field (36..41).
        #include <Common/GIFrame.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>
        layout(std430, binding = 10) readonly buffer GIFrameUB
        {
        	GIFrame giFrame;
        };
        layout(std430, binding = 11) readonly buffer PointLightsUB
        {
        	PointLight pointLights[];
        };
        layout(std430, binding = 12) readonly buffer SpotLightsUB
        {
        	SpotLight spotLights[];
        };
        layout(std430, binding = 13) readonly buffer LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        layout(std430, binding = 14) readonly buffer LightGridWorldCellsUB
        {
        	uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>
        layout(std430, binding = 15) readonly buffer RadianceCacheParamsUB
        {
        	RadianceCacheParams radianceCache;
        };
        layout(std430, binding = 16) readonly buffer RadianceCacheIndirectionUB
        {
        	uint radianceCacheIndirection[];
        };
        layout(std430, binding = 17) readonly buffer RadianceCacheProbesUB
        {
        	RadianceCacheProbe radianceCacheProbes[];
        };
        layout(binding = 18) uniform sampler2D u_RadianceCacheIrradiance;
        layout(binding = 19) uniform sampler2D u_RadianceCacheDepth;
        layout(binding = 20) uniform samplerCube u_EnvSpecularTex;
        layout(binding = 21) uniform sampler2D u_GBufferA;
        layout(binding = 22) uniform sampler2D u_GBufferEmissive;
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 23
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 29
        #include <Common/GIHitLighting.glslh>
        #if defined( DESERT_GI_TRACER_SOFTWARE )
        #define GI_MESH_SDF_BINDING 36
        #include <Common/GITraceMeshSDF.glslh>
        #endif

        // UE r.Lumen.Reflections.MaxRoughnessToTrace (0.4) and its fade (r.Lumen.Reflections.RoughnessFadeLength,
        // 0.1): a texel this rough or less sends its rays past the screen; rougher, the screen alone (as ScreenSpace).
        const float kTracedMaxRoughness  = 0.4;
        const float kTracedRoughnessFade = 0.1;
        // How far a ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance; the screen probes' own).
        const float kTracedMaxDistance = 20000.0;
        // cm the near field's origin leaves the surface along the normal and the ray (UE's mesh SDF SurfaceBias;
        // ScreenProbeTrace.shader's).
        const float kMeshSDFBias = 5.0;
        // The near field's objects a tile keeps.
        #define SSR_MESH_SDF_MAX_OBJECTS 64u

        shared uint s_TileBoxMin[3]; // the tile's traced surfaces' world box, as order-preserving uints
        shared uint s_TileBoxMax[3];
        shared uint s_MeshSDFCount;
        shared uint s_MeshSDFObjects[SSR_MESH_SDF_MAX_OBJECTS];
        shared vec4 s_MeshSDFBoxMin[SSR_MESH_SDF_MAX_OBJECTS]; // world box, w = its distance from the tile's box (cm)
        shared vec3 s_MeshSDFBoxMax[SSR_MESH_SDF_MAX_OBJECTS];
        shared uint s_MeshSDFOrder[SSR_MESH_SDF_MAX_OBJECTS];  // the slots nearest the tile first

        // A float as a uint whose unsigned order is the float's (atomicMin / atomicMax over shared floats).
        uint OrderedFromFloat(float f)
        {
        	const uint u = floatBitsToUint(f);
        	return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
        }
        float FloatFromOrdered(uint u)
        {
        	return uintBitsToFloat((u & 0x80000000u) != 0u ? (u & 0x7FFFFFFFu) : ~u);
        }

        // The segment [tMin, tMax] of the ray against a world box: false when it misses.
        bool RayHitsBox(vec3 origin, vec3 invDir, float tMin, float tMax, vec3 boxMin, vec3 boxMax)
        {
        	const vec3  a    = (boxMin - origin) * invDir;
        	const vec3  b    = (boxMax - origin) * invDir;
        	const vec3  lo   = min(a, b);
        	const vec3  hi   = max(a, b);
        	const float near = max(max(lo.x, lo.y), max(lo.z, tMin));
        	const float far  = min(min(hi.x, hi.y), min(hi.z, tMax));
        	return near <= far;
        }

        // The world legs of one ray from the surface @p worldPos (normal @p N) along @p L, starting @p start cm along
        // it (where the screen was certain): the tile's mesh SDFs out to @p nearEnd cm (when @p nearField),
        // then the tracer to kTracedMaxDistance. True on a hit: @p radiance = GIHitRadiance, @p hitPoint its position.
        bool TraceWorld(vec3 worldPos, vec3 N, vec3 L, float start, bool nearField, float nearEnd, out vec3 radiance,
                        out vec3 hitPoint)
        {
        	radiance = vec3(0.0);
        	hitPoint = worldPos;
        	GITraceInput ray;
        	ray.origin    = worldPos + N * GIRayBias(worldPos, N, L).normalOffset;
        	ray.tMin      = 0.0;
        	ray.direction = L;
        	ray.tMax      = kTracedMaxDistance;
        	ray.coneAngle = 0.0;
        	ray.flags     = 0u;
        #if defined( DESERT_GI_TRACER_SOFTWARE )
        	if (nearField)
        	{
        		const vec3  nearOrigin = worldPos + (N + L) * kMeshSDFBias;
        		if (start < nearEnd)
        		{
        			GIMeshSDFTrace near;
        			near.hitDistance = nearEnd;
        			near.hitObject   = 0u;
        			const uint objects = min(s_MeshSDFCount, SSR_MESH_SDF_MAX_OBJECTS);
        			const vec3 invDir  = vec3(L.x != 0.0 ? 1.0 / L.x : 3.4e38, L.y != 0.0 ? 1.0 / L.y : 3.4e38,
        			                          L.z != 0.0 ? 1.0 / L.z : 3.4e38);
        			for (uint k = 0u; k < objects; ++k)
        			{
        				const uint slot = s_MeshSDFOrder[k];
        				if (!RayHitsBox(nearOrigin, invDir, start, near.hitDistance, s_MeshSDFBoxMin[slot].xyz,
        				                s_MeshSDFBoxMax[slot]))
        					continue;
        				GITraceMeshSDFObject(nearOrigin, L, start, nearEnd, s_MeshSDFObjects[slot], near);
        			}
        			if (near.hitDistance < nearEnd)
        			{
        				const GIHit hit = GIMeshSDFHit(nearOrigin, L, 0.0, near);
        				radiance        = GIHitRadiance(hit, L);
        				hitPoint        = hit.position;
        				return true;
        			}
        		}
        		ray.origin = nearOrigin;
        		start      = max(start, nearEnd);
        	}
        #endif
        	ray.tMin = min(start, ray.tMax);
        	const GITraceResult far = GITraceRay(ray);
        	if (!far.hit)
        		return false;
        	radiance = GIHitRadiance(far.surface, L);
        	hitPoint = far.surface.position;
        	return true;
        }
        #endif // DESERT_REFLECTIONS_TRACED

        // Filled on the CPU from the view's matrices once per view frame (SSRRenderer::UploadTraceUniforms).
        // u_InvJitteredViewProjection = inverse of the matrix the G-buffer depth was rasterised with
        // (Common/ReconstructPosition.glslh); u_JitteredViewProjection = that matrix (rays are projected with it,
        // so a ray and the depth it is tested against are in one space). Where a hit was on last frame's screen
        // is the hit pixel's VELOCITY, not a camera reprojection: a moving reflected object is read where it
        // was, not where the still world behind it was (UE SSRT: the hit is reprojected through the velocity).
        Uniform(6) SSRTraceUB
        {
        	mat4 u_InvJitteredViewProjection;
        	mat4 u_JitteredViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 u_CameraPos;  // xyz = camera world position, w = per-frame seed (ray sequence + 2x2 rotation)
        	vec4 u_SSRParams;  // x = rays per texel, y = HZB iterations per ray, z = intensity, w = thickness (world)
        	vec4 u_SSRParams2; // x = max ray distance (world), y = 1 when u_SceneColor holds last frame, zw unused
        };

        LocalSize(8, 8, 1);

        shared uint s_TileReflects;

        const float kPi = 3.14159265358979;
        // A miss's point: 1 km (centimetres) along its ray - far enough that a neighbour re-aiming at it draws the
        // same direction (the environment is at infinity), near enough to stay exact in a 32-bit float.
        const float kMissDistance = 1.0e5;

        // Per-pixel hash: decorrelates neighbouring texels' ray sequences.
        float hash12(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // The R2 low-discrepancy sequence (Roberts 2018): sample n of a 2D sequence that fills the square evenly
        // for every prefix length, offset per pixel by a Cranley-Patterson rotation. Frames and rays share one index,
        // so the denoiser's temporal accumulation gathers successive, well-spread lobe samples.
        vec2 RayRandom(vec2 rotation, uint index)
        {
        	return fract(rotation + vec2(0.7548776662, 0.5698402910) * float(index));
        }

        // Orthonormal basis around n (Duff et al. 2017, "Building an Orthonormal Basis, Revisited").
        void Basis(vec3 n, out vec3 t, out vec3 b)
        {
        	const float s = n.z >= 0.0 ? 1.0 : -1.0;
        	const float a = -1.0 / (s + n.z);
        	const float c = n.x * n.y * a;
        	t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
        	b = vec3(c, s + n.y * n.y * a, -n.y);
        }

        // Heitz 2018, "Sampling the GGX Distribution of Visible Normals" (UE ImportanceSampleVisibleGGX): a microfacet
        // normal in tangent space seen from ve (tangent space, z = the surface normal), alpha = roughness^2.
        vec3 SampleGGXVNDF(vec3 ve, float alpha, vec2 u)
        {
        	const vec3  vh    = normalize(vec3(alpha * ve.x, alpha * ve.y, ve.z));
        	const float lensq = vh.x * vh.x + vh.y * vh.y;
        	const vec3  t1    = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
        	const vec3  t2    = cross(vh, t1);
        	const float r     = sqrt(u.x);
        	const float phi   = 2.0 * kPi * u.y;
        	const float p1    = r * cos(phi);
        	const float s     = 0.5 * (1.0 + vh.z);
        	const float p2    = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * r * sin(phi);
        	const vec3  nh    = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
        	return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
        }

        // log2 of the pdf with which SampleGGXVNDF + reflect draws the direction Lt (tangent space) for the view
        // Ve: D(H) * G1(V) / (4 * N.V) (Heitz 2018 eq. 17 through the reflection Jacobian). The reconstruction divides
        // its own BRDF by it to reuse this texel's estimate (Stachowiak 2015). log2: the pdf of a smooth lobe
        // exceeds half-float range.
        float Log2VNDFReflectionPdf(vec3 Ve, vec3 Lt, float alpha)
        {
        	const vec3  H   = normalize(Ve + Lt);
        	const float a2  = alpha * alpha;
        	const float d   = H.z * H.z * (a2 - 1.0) + 1.0;
        	const float D   = a2 / (kPi * d * d);
        	const float nDotV = max(Ve.z, 1e-4);
        	const float G1V = 2.0 * nDotV / (nDotV + sqrt(a2 + (1.0 - a2) * nDotV * nDotV));
        	return log2(max(D * G1V / (4.0 * nDotV), 1e-30));
        }

        // Smith's masking term for GGX, G1(l) - the separable Smith G2 / G1(v) that is left of BRDF * cos / pdf once
        // the visible-normal pdf has cancelled D and G1(v) (Heitz 2018 eq. 19), Fresnel excluded (the split sum's).
        float SmithG1(float nDotL, float alpha)
        {
        	const float a2 = alpha * alpha;
        	return 2.0 * nDotL / (nDotL + sqrt(a2 + (1.0 - a2) * nDotL * nDotL));
        }

        // World point -> (uv, deviceZ) through the jittered matrix. False behind the camera.
        bool ProjectToScreen(mat4 viewProj, vec3 p, out vec3 screen)
        {
        	const vec4 clip = viewProj * vec4(p, 1.0);
        	if (clip.w <= 0.0)
        		return false;
        	screen = vec3(clip.xy / clip.w * 0.5 + 0.5, clip.z / clip.w);
        	screen.y = 1.0 - screen.y; // engine renders through a Y-flipped viewport (Common/ScreenUV.glslh)
        	return true;
        }

        // How much a walk that stopped at @p hit (uv, deviceZ) is a real reflection of the surface there, in [0, 1].
        // 0 off the screen and on sky texels; 0 where the ray passed BEHIND the surface by more than the thickness
        // (the depth buffer cannot say what is behind its first layer - the next source answers it); 0 on a face
        // the ray arrives at from behind; then UE's screen-edge vignette and a soft end at the max distance.
        // @p surface = the world point of the depth buffer at the hit (meaningful where the result is > 0).
        float SSRHitConfidence(vec3 hit, vec3 origin, vec3 L, float thickness, float maxDistance, out vec3 surface)
        {
        	surface = origin;
        	if (any(lessThan(hit.xy, vec2(0.0))) || any(greaterThan(hit.xy, vec2(1.0))))
        		return 0.0;
        	const ivec2 size  = textureSize(u_GBufferDepth, 0);
        	const ivec2 texel = clamp(ivec2(hit.xy * vec2(size)), ivec2(0), size - 1);
        	const vec3  hN    = texelFetch(u_GBufferNormal, texel, 0).rgb;
        	if (dot(hN, hN) <= 0.001)
        		return 0.0; // sky: the sky is the environment's, not a screen hit
        	if (dot(normalize(hN), L) > 0.0)
        		return 0.0; // back face
        	surface             = ReconstructWorldPosition(hit.xy, texelFetch(u_GBufferDepth, texel, 0).r,
        	                                               u_InvJitteredViewProjection);
        	const vec3  rayAt   = ReconstructWorldPosition(hit.xy, hit.z, u_InvJitteredViewProjection);
        	const float behind  = distance(surface, rayAt);
        	float confidence    = 1.0 - smoothstep(0.0, thickness, behind);
        	confidence *= confidence;
        	const float travel = distance(origin, surface);
        	return confidence * HZBScreenEdgeFade(hit.xy) * (1.0 - smoothstep(0.7 * maxDistance, maxDistance, travel));
        }

        // The traced texel gPix (HALF RESOLUTION: one G-buffer pixel of a 2x2 block, rotating every frame).
        // Returns rgb = the ratio-estimated radiance of the hits, a = coverage (see the header); @p ray = the
        // texel's ray for the reconstruction (u_TraceRay): xyz = the point it hit (the weighted mean of the hit points;
        // when nothing hit, a point kMissDistance along the weighted mean direction of all rays - the environment
        // the next source answers is at infinity), w = the log2 pdf of drawing the direction to it from here.
        // A texel with no ray: pdf 0 (log -126), which the reconstruction reads as "nothing to reuse".
        // @p nearField (traced only): the texel's rays may march the tile's mesh SDFs.
        vec4 TraceFrom(ivec2 gPix, ivec2 gSize, bool nearField, out vec4 ray)
        {
        	ray = vec4(0.0, 0.0, 0.0, -126.0);
        	// Whether the screen holds last frame to return at a hit. Without it the screen-only trace answers
        	// nothing (all to the next source); the traced one still has the world.
        	const bool screenLeg = u_SSRParams2.y >= 0.5;
        #if !defined( DESERT_REFLECTIONS_TRACED )
        	if (!screenLeg)
        		return vec4(0.0);
        #endif

        	const vec2 gUV = (vec2(gPix) + 0.5) / vec2(gSize);
        	const vec4 gb  = texelFetch(u_GBufferNormal, gPix, 0);
        	if (!SSRPixelCanReflect(gb))
        		return vec4(0.0);
        	const vec3  N         = normalize(gb.rgb);
        	const float roughness = gb.a;
        	const float alpha     = max(roughness * roughness, 0.001);

        	const vec3 worldPos = ReconstructWorldPosition(gUV, texelFetch(u_GBufferDepth, gPix, 0).r,
        	                                               u_InvJitteredViewProjection);
        	const vec3 V        = normalize(u_CameraPos.xyz - worldPos);
        	vec3       T, B;
        	Basis(N, T, B);
        	const vec3 Ve = vec3(dot(V, T), dot(V, B), dot(V, N));

        	// The ray leaves 2 cm off the surface (a world distance: centimetres) so a grazing ray does not meet its
        	// own texel's neighbours in the depth buffer.
        	const vec3 origin = worldPos + N * 2.0;
        	vec3       originScreen;
        	const bool onScreen = ProjectToScreen(u_JitteredViewProjection, origin, originScreen);
        #if !defined( DESERT_REFLECTIONS_TRACED )
        	if (!onScreen)
        		return vec4(0.0);
        #endif
        	const float originW = (u_JitteredViewProjection * vec4(origin, 1.0)).w;

        	const int   rays        = int(u_SSRParams.x);
        	const int   iterations  = int(u_SSRParams.y);
        	const float thickness   = u_SSRParams.w;
        	const float maxDistance = u_SSRParams2.x;
        	const vec2  rotation    = vec2(hash12(vec2(gPix)), hash12(vec2(gPix) + 17.17));
        	const uint  seed        = uint(u_CameraPos.w);
        #if defined( DESERT_REFLECTIONS_TRACED )
        	// UE MaxRoughnessToTrace: the share of this texel's rays that go past the screen, dithered per ray.
        	const float worldShare =
        	     1.0 - smoothstep(kTracedMaxRoughness - kTracedRoughnessFade, kTracedMaxRoughness, roughness);
        #endif

        	vec3  radiance  = vec3(0.0); // sum of w * conf * L, Karis-weighted
        	float hitWeight = 0.0;       // sum of w * conf, Karis-weighted (the radiance's normaliser)
        	float coverage  = 0.0;       // sum of w * conf
        	float weightSum = 0.0;       // sum of w over every ray
        	vec3  hitPoint  = vec3(0.0); // sum of w * conf * surface: the texel's hit point (world)
        	vec3  rayDir    = vec3(0.0); // sum of w * Lt over every ray: the texel's direction when nothing hit
        #if defined( DESERT_REFLECTIONS_TRACED )
        	// The rays the screen left to the world share ONE world ray (UE Lumen reflections: one traced ray per
        	// texel): drawn among them in proportion to their weight (a reservoir), its answer stands for all of them,
        	// and the temporal accumulation averages the draw over frames.
        	float worldWeight = 0.0;      // sum of w of the rays left to the world
        	vec3  worldL      = vec3(0.0); // the drawn ray's direction
        	float worldStart  = 0.0;       // and where its world legs start
        #endif
        	for (int r = 0; r < rays; ++r)
        	{
        		const vec2 u = RayRandom(rotation, seed * uint(rays) + uint(r));
        		const vec3 H = SampleGGXVNDF(Ve, alpha, u);
        		vec3       Lt = reflect(-Ve, H);
        		if (Lt.z <= 0.0)
        			continue; // under the surface: a lobe sample that reflects nothing (weight 0 in the estimator)
        		const vec3  L = normalize(Lt.x * T + Lt.y * B + Lt.z * N);
        		const float w = SmithG1(Lt.z, alpha);
        		weightSum += w;
        		rayDir    += w * Lt;

        		// THE SCREEN. The screen direction: the ray's end a short way along (a quarter of the origin's view
        		// depth, cut short of the camera plane), projected. The walk extends it to the screen edge.
        		float confidence = 0.0;
        		vec3  surface    = origin;
        		vec3  L_i        = vec3(0.0);
        		float start      = 0.0; // traced: where the world legs start along the ray (0: uncertain)
        		const float dw   = (u_JitteredViewProjection * vec4(L, 0.0)).w;
        		float rayLength  = 0.25 * originW;
        		if (dw < 0.0)
        			rayLength = min(rayLength, 0.5 * originW / -dw);
        		vec3 endScreen;
        		if (screenLeg && onScreen && ProjectToScreen(u_JitteredViewProjection, origin + L * rayLength, endScreen))
        		{
        			const HZBTraceResult walk = HZBTraceScreen(originScreen, endScreen - originScreen, iterations);
        			if (walk.Exhausted)
        			{
        #if defined( DESERT_REFLECTIONS_TRACED )
        				// Off the screen in front of every surface it passed: certain that far (UE writes the distance).
        				const bool offScreen = any(lessThan(walk.Position.xy, vec2(0.0))) ||
        				                       any(greaterThan(walk.Position.xy, vec2(1.0)));
        				if (offScreen && walk.Position.z > 0.0 && walk.Position.z < 1.0)
        				{
        					const vec3 exitWorld = ReconstructWorldPosition(clamp(walk.Position.xy, 0.0, 1.0),
        					                                                walk.Position.z, u_InvJitteredViewProjection);
        					start = max(distance(origin, exitWorld) - 2.0 * kMeshSDFBias, 0.0);
        				}
        #endif
        			}
        			else
        			{
        				confidence = SSRHitConfidence(walk.Position, origin, L, thickness, maxDistance, surface);
        				if (confidence > 0.0)
        				{
        					// The hit's radiance: where the surface point was on LAST frame's screen - the hit pixel
        					// minus its velocity (uv = ndc * (0.5, -0.5) under the engine's flipped viewport,
        					// View/Velocity.hpp). The velocity carries the object's own motion as well as the camera's,
        					// so a car passing in a shop window is read where the car was, not where the street behind
        					// it was: no trail behind moving objects.
        					const ivec2 vSize  = textureSize(u_Velocity, 0);
        					const ivec2 vTexel = clamp(ivec2(walk.Position.xy * vec2(vSize)), ivec2(0), vSize - 1);
        					const vec2  prevUV = walk.Position.xy - texelFetch(u_Velocity, vTexel, 0).xy * vec2(0.5, -0.5);
        					if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
        						confidence = 0.0;
        					else
        						L_i = textureLod(u_SceneColor, prevUV, 0.0).rgb;
        #if defined( DESERT_REFLECTIONS_TRACED )
        					// The depth there was certain: a world leg starts just before the surface the screen found.
        					start = max(distance(origin, surface) - 2.0 * kMeshSDFBias, 0.0);
        #endif
        				}
        			}
        		}
        #if defined( DESERT_REFLECTIONS_TRACED )
        		// THE WORLD, where the screen did not answer (UE: the vignette and the thickness as a dither, so a
        		// partly confident screen hit hands the rest of its rays over instead of fading them out).
        		const float dither = hash12(vec2(gPix) + vec2(float(seed), float(r)) * 7.31);
        		if (worldShare > dither && confidence <= dither)
        		{
        			worldWeight += w;
        			if (hash12(vec2(gPix) * 1.37 + vec2(float(r), float(seed)) * 3.11) * worldWeight < w)
        			{
        				worldL     = L;
        				worldStart = start;
        			}
        			continue;
        		}
        #endif
        		if (confidence <= 0.0)
        			continue;

        		// Karis' inverse-luminance weight inside the estimator: a single ray onto a sun-lit wall does not
        		// become a firefly the temporal accumulation has to average away over many frames.
        		const float karis = 1.0 / (1.0 + dot(L_i, vec3(0.2126, 0.7152, 0.0722)));
        		radiance  += L_i * (w * confidence * karis);
        		hitWeight += w * confidence * karis;
        		coverage  += w * confidence;
        		hitPoint  += (w * confidence) * surface;
        	}
        #if defined( DESERT_REFLECTIONS_TRACED )
        	if (worldWeight > 0.0)
        	{
        		vec3 L_i;
        		vec3 surface;
        		if (TraceWorld(worldPos, N, worldL, worldStart, nearField, u_SSRParams2.z, L_i, surface))
        		{
        			const float karis = 1.0 / (1.0 + dot(L_i, vec3(0.2126, 0.7152, 0.0722)));
        			radiance  += L_i * (worldWeight * karis);
        			hitWeight += worldWeight * karis;
        			coverage  += worldWeight;
        			hitPoint  += worldWeight * surface;
        		}
        	}
        #endif
        	if (weightSum <= 0.0)
        		return vec4(0.0);
        	if (hitWeight <= 0.0)
        	{
        		const vec3 Lt = normalize(rayDir);
        		ray = vec4(worldPos + normalize(Lt.x * T + Lt.y * B + Lt.z * N) * kMissDistance,
        		           Log2VNDFReflectionPdf(Ve, Lt, alpha));
        		return vec4(0.0);
        	}
        	const vec3 hit = hitPoint / coverage;
        	const vec3 L   = normalize(hit - worldPos);
        	ray = vec4(hit, Log2VNDFReflectionPdf(Ve, vec3(dot(L, T), dot(L, B), max(dot(L, N), 1e-4)), alpha));

        	const float fade = SSRRoughnessFade(roughness) * u_SSRParams.z;
        	return vec4(radiance / hitWeight, clamp(coverage / weightSum * fade, 0.0, 1.0));
        }

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        	{
        		s_TileReflects = 0u;
        #if defined( DESERT_REFLECTIONS_TRACED )
        		s_MeshSDFCount = 0u;
        		for (int a = 0; a < 3; ++a)
        		{
        			s_TileBoxMin[a] = 0xFFFFFFFFu;
        			s_TileBoxMax[a] = 0u;
        		}
        #endif
        	}
        	barrier();

        	// --- Classify: this tile spans [floor(t * scale), ceil((t + 1) * scale)) texels, up to 9, hence up
        	// to 2x2 texels per thread; the verdict is reduced in shared memory. ---
        	ivec2 full  = textureSize(u_GBufferNormal, 0);
        	ivec2 grid  = imageSize(u_TileMask);
        	ivec2 tile  = ivec2(gl_WorkGroupID.xy);
        	vec2  scale = vec2(full) / vec2(grid);
        	ivec2 p0    = ivec2(floor(vec2(tile) * scale));
        	ivec2 p1    = min(ivec2(ceil(vec2(tile + 1) * scale)), full);

        	bool reflects = false;
        	for (int oy = 0; oy < 2; oy++)
        		for (int ox = 0; ox < 2; ox++)
        		{
        			ivec2 p = p0 + ivec2(gl_LocalInvocationID.xy) + ivec2(ox, oy) * 8;
        			if (p.x < p1.x && p.y < p1.y && SSRPixelCanReflect(texelFetch(u_GBufferNormal, p, 0)))
        				reflects = true;
        		}
        	if (reflects)
        		atomicOr(s_TileReflects, 1u);
        	barrier();

        	const bool tileReflects = s_TileReflects != 0u;
        	if (gl_LocalInvocationIndex == 0u)
        		imageStore(u_TileMask, tile, vec4(tileReflects ? 1.0 : 0.0));

        	// --- Trace: the half-resolution texels whose source pixel THIS tile owns. A pixel's owner is the tile
        	// whose quad rasterizes it (floor((x + 0.5) / scale)); the stretched ranges above overlap by a pixel,
        	// ownership does not, so every trace texel is written by exactly one thread. 8 texels per axis from
        	// (p0 - 1) / 2 reach source pixels p0 - 2 .. p0 + 15, beyond the 9 a tile can own. ---
        	ivec2 traceSize = imageSize(u_Trace);
        	int   frame     = int(u_CameraPos.w);
        	ivec2 q         = max((p0 - 1) / 2, ivec2(0)) + ivec2(gl_LocalInvocationID.xy);
        	ivec2 src       = min(q * 2 + ivec2(frame & 1, (frame >> 1) & 1), full - 1);
        	const bool owns = q.x < traceSize.x && q.y < traceSize.y && ivec2(floor((vec2(src) + 0.5) / scale)) == tile;

        	bool nearField = false;
        #if defined( DESERT_REFLECTIONS_TRACED ) && defined( DESERT_GI_TRACER_SOFTWARE )
        	// --- THE TILE'S MESH SDF OBJECTS (UE CullMeshSDFObjectsToGrid, per tile here as per probe in
        	// ScreenProbeTrace.shader): the view-culled objects whose box lies within the near field's reach of the
        	// world box of the tile's traced surfaces that send rays past the screen, nearest first. ---
        	if (owns && tileReflects)
        	{
        		const vec4 gb = texelFetch(u_GBufferNormal, src, 0);
        		const vec3 P  = ReconstructWorldPosition((vec2(src) + 0.5) / vec2(full), texelFetch(u_GBufferDepth, src, 0).r,
        		                                         u_InvJitteredViewProjection);
        		nearField = SSRPixelCanReflect(gb) && gb.a < kTracedMaxRoughness &&
        		            distance(P, u_CameraPos.xyz) <= u_SSRParams2.w;
        		if (nearField)
        			for (int a = 0; a < 3; ++a)
        			{
        				atomicMin(s_TileBoxMin[a], OrderedFromFloat(P[a]));
        				atomicMax(s_TileBoxMax[a], OrderedFromFloat(P[a]));
        			}
        	}
        	barrier();
        	const bool tileNear = s_TileBoxMin[0] <= s_TileBoxMax[0];
        	if (tileNear)
        	{
        		const vec3  tileMin = vec3(FloatFromOrdered(s_TileBoxMin[0]), FloatFromOrdered(s_TileBoxMin[1]),
        		                           FloatFromOrdered(s_TileBoxMin[2]));
        		const vec3  tileMax = vec3(FloatFromOrdered(s_TileBoxMax[0]), FloatFromOrdered(s_TileBoxMax[1]),
        		                           FloatFromOrdered(s_TileBoxMax[2]));
        		const float reach   = u_SSRParams2.z + 2.0 * kMeshSDFBias;
        		const uint  count   = GIMeshSDFCulledCount();
        		for (uint i = gl_LocalInvocationIndex; i < count; i += 64u)
        		{
        			vec3       boxMin, boxMax;
        			const uint object = GIMeshSDFCulledObject(i, boxMin, boxMax);
        			const vec3 gap    = max(max(boxMin - tileMax, tileMin - boxMax), vec3(0.0));
        			if (dot(gap, gap) > reach * reach)
        				continue;
        			const uint slot = atomicAdd(s_MeshSDFCount, 1u);
        			if (slot < SSR_MESH_SDF_MAX_OBJECTS)
        			{
        				s_MeshSDFObjects[slot] = object;
        				s_MeshSDFBoxMin[slot]  = vec4(boxMin, length(gap));
        				s_MeshSDFBoxMax[slot]  = boxMax;
        			}
        		}
        	}
        	barrier();
        	// Nearest first (a rank per slot): a ray's first hit then clips the objects behind it to nothing.
        	{
        		const uint kept = min(s_MeshSDFCount, SSR_MESH_SDF_MAX_OBJECTS);
        		const uint self = gl_LocalInvocationIndex;
        		if (self < kept)
        		{
        			const float own  = s_MeshSDFBoxMin[self].w;
        			uint        rank = 0u;
        			for (uint j = 0u; j < kept; ++j)
        			{
        				const float other = s_MeshSDFBoxMin[j].w;
        				rank += (other < own || (other == own && j < self)) ? 1u : 0u;
        			}
        			s_MeshSDFOrder[rank] = self;
        		}
        	}
        	barrier();
        	nearField = nearField && s_MeshSDFCount > 0u;
        #endif

        	if (!owns)
        		return;
        	vec4 ray   = vec4(0.0, 0.0, 0.0, -126.0);
        	vec4 trace = tileReflects ? TraceFrom(src, full, nearField, ray) : vec4(0.0);
        	imageStore(u_Trace, q, trace);
        	imageStore(u_TraceRay, q, ray);
        }
    }
}

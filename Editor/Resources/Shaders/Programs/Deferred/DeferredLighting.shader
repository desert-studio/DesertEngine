// DesertAsset {"Kind":"Shader","Guid":"0de6e7b591a1fa8e26876c3c40dfc363","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DeferredLighting"
{
    // Deferred lighting + G-buffer debug visualization (fullscreen). Consumes the scene renderer's MRT G-buffer.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        // Deferred lighting + G-buffer DEBUG visualization (fullscreen). Reads the G-buffer (albedo/metallic,
        // world-normal/roughness, world-position) and shades it with the directional sun (cascaded shadow-mapped) +
        // all point & spot lights (the deferred payoff: many dynamic lights in screen space). In a debug mode it
        // instead shows a raw G-buffer channel full-screen. Non-geometry texels are discarded so the LOADed forward
        // scene (real procedural sky / skybox + grid) shows through.

        #include <Mesh/PointLight.glslh>      // binding 6  (SSBO PointLightsUB) + BRDF
        #include <Mesh/Spotlight.glslh>       // binding 16 (SSBO SpotLightsUB)
        #include <Mesh/LightsMetadata.glslh>  // binding 4  (UB LightsMetadata: point/spot/dir counts)
        // THE direct-light BRDF, shared with the forward mesh shaders and with the point/spot headers
        // above (which already include it). Named explicitly because this pass calls it directly.
        #include <Mesh/DirectLighting.glslh>
        // World position from the G-buffer depth (u_GBufferDepth below): the one reconstruction every deferred
        // reader shares.
        #include <Common/ReconstructPosition.glslh>

        In(0) vec2 v_TexCoord;

        // The G-buffer's layout and the shading-model ids it carries (the shading word, Emissive.a).
        // The shading word (ShadingModels/ShadingModelContract.glslh): an RG32_UINT target, read ONLY by texelFetch.
        Uniform(1) usampler2D u_GBufferShadingWord;
        // Device depth (nearest sampler: DEPTH32F is not filtered); world position = ReconstructWorldPosition.
        Uniform(21) sampler2D u_GBufferDepth;
        Uniform(2) sampler2D u_GBufferA; // rgb = albedo, a = metallic
        Uniform(3) sampler2D u_GBufferB; // rgb = world normal, a = roughness
        // The denoised GTAO (Programs/Deferred/GTAO.shader through ScreenSpaceDenoiser): rgb = the bent normal encoded
        // 0.5 + 0.5 n, a = visibility (1 = unoccluded). System.White and u_Params.z = 0 when AO is off.
        Uniform(8) sampler2D u_SSAO;
        Uniform(9) sampler2D u_GBufferEmissive; // rgb = HDR emissive (added below), a = material AO
        // The one-bounce indirect light, denoised (ScreenSpaceDenoiser, diffuse indirect) and with its intensity
        // applied by its producer: the SSGI trace (Programs/Deferred/SSGI.shader - the previous frame's lit scene,
        // every light) or the RSM gather (GIResolve.shader - the sun). System.Black when GI is off.
        Uniform(10) sampler2D u_GI;

        // THE BAKED ENVIRONMENT — the same trio the forward mesh shaders bind, and the whole point of
        // this pass having an ambient term at all. Until 2026-09-03 the deferred composite read NEITHER
        // cube and floored its ambient at a flat vec3(0.08), so 51 of the repository's 53 scenes lit
        // their ordinary static opaque geometry from a constant while the skinned meshes, the
        // custom-shader meshes and the glass drawn forward over this very composite lit themselves from
        // the sky. One material was shaded two ways depending on whether its mesh had a skeleton.
        //
        // New slots (17..19) rather than the forward path's 8..10: those three are already taken here by
        // u_SSAO / u_GBufferEmissive / u_GI, and this pass shares no descriptor layout with the mesh
        // shaders — it is a fullscreen triangle with its own material. Only the NAMES have to match, because
        // the material binds by name.
        Uniform(17) samplerCube u_EnvIrradianceTex; // diffuse irradiance (cosine-convolved sky)
        Uniform(18) samplerCube u_EnvSpecularTex;   // GGX-prefiltered radiance, roughness across mips
        Uniform(19) sampler2D   u_BRDFLUTTexture;   // split-sum BRDF integration (cosLo, roughness)
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). Slot 20: the
        // first free one in this layout.
        Uniform(20) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;

        Out(0) vec4 oColor;

        const vec3 Fdielectric = vec3(0.04); // base reflectance for dielectrics (matches Lit.glsl.frag)

        // The ambient model itself, shared verbatim with StaticMeshLit / StaticMeshLit_Instanced /
        // SkinnedMeshLit. Included after the three bindings above because it names them.
        // The reflection captures (Common/ReflectionEnvironment.glslh): the per-capture records and their cube array,
        // bound by the Composite node as scene view inputs (SceneViewInputs::ReflectionCaptures / ...Cubes).
        // Slots 22 and 23: the first free ones in this layout.
        #define DESERT_REFLECTION_CAPTURES
        #include <Common/ReflectionCaptureGpu.glslh>
        ReadBuffer(22) ReflectionCapturesUB
        {
            uvec4                u_ReflectionCaptureHeader; // x = capture count
            ReflectionCaptureGpu u_ReflectionCaptures[];
        };
        Uniform(23) samplerCubeArray u_ReflectionCaptureCubes;
        // The screen-space reflection of this frame (SSRRenderer's resolved trace, full resolution): rgb = the traced
        // radiance, a = the share of the specular lobe the trace answered. When SSR is off it is System.Black (opaque)
        // and u_CameraPos.w = 0: nothing is read, the environment answers the whole lobe. Slot 24: the next free one.
        Uniform(24) sampler2D u_SSR;
        #include <Mesh/AmbientIBL.glslh>
        #include <Mesh/LightSources.glslh>
        // THE lighting of this pass: the texel's shading model through the dispatch the registry generates. The
        // same two calls as the forward pass (Mesh/Surface/Pass_Forward.glslh); this shader names no model.
        #include <Generated/ShadingModels.glslh>

        // The directional (sun) Cook-Torrance contribution used to be a local copy of the BRDF right here,
        // with a comment recording that the raw albedo*NdotL*intensity Lambert made the sun ~PI× too bright
        // and washed out the point/spot lights. The forward mesh shaders had shipped exactly that raw form
        // for as long, so the comment described a defect that was live one file away. The copy is gone: the
        // sun, like every point and spot light, is a DesertLight (Mesh/LightSources.glslh) handed to the
        // surface's shading model, whose DefaultLit body is EvaluateDirectLight from Mesh/DirectLighting.glslh.

        Uniform(0) DeferredUB
        {
        	vec4 u_LightDir;   // xyz = direction the light travels (away from the sun); w unused
        	vec4 u_LightColor; // rgb = colour, a = intensity
        	vec4 u_Params;     // x = debug mode (0..9), z = 1 when u_SSAO holds this frame's GTAO; y, w unused
        	vec4 u_CameraPos;  // xyz = camera world position (for the view vector); w = 1 when u_SSR holds a trace
        	mat4 u_InvJitteredViewProjection; // the inverse of the matrix the G-buffer depth was rasterised with
        };

        // The cascaded shadow's two bindings, at slots free in THIS layout. The text that reads them is
        // one file for every shader in the engine that shades a surface with the sun —
        // Mesh/CascadedShadow.glslh, included right below because it names what is declared here.
        Uniform(5) sampler2DArray u_ShadowCascades; // one layer per cascade
        Uniform(7) ShadowUB {
        	mat4 u_LightViewProj[4];
        	vec4 u_ShadowParams;      // x = tan(sun source angle / 2), y = enabled (>0.5), z = debug mode (0/1/2), w = cascade count
        	vec4 u_DebugParams;       // x = show normals (>0.5), y = light debug (>0.5); z,w reserved
        	vec4 u_CascadeTexelWorld; // per-cascade world size of one shadow-map texel (x..w = cascade 0..3)
        	vec4 u_CascadeDepthRange; // per-cascade world length the [0,1] map depth spans (x..w = cascade 0..3)
        };

        #include <Mesh/CascadedShadow.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — a second, independent occluder of the same sun.
        // Declared here, evaluated by the shared receiver text below: the wrapper used to live in this
        // file and this file ALONE, which is exactly why no forward-shaded surface in the engine ever
        // received a cloud shadow. See Common/CloudShadowReceiver.glslh.
        Uniform(11) sampler2D u_CloudShadowMap;
        Uniform(12) CloudShadowUB {
        	mat4 u_CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 u_CloudShadowParams;
        };

        // THE receiver, shared verbatim with the forward mesh shaders, the glass pass and the terrain.
        // Included after the two bindings above because it names them.
        #include <Common/CloudShadowReceiver.glslh>

        // THE POINT AND SPOT LIGHTS' SHADOWS: the view's local shadow atlas and the table of its views
        // (Graphic/LocalShadows.hpp; MeshRenderer::UpdateLocalShadows writes both, and each light's shadowView).
        // The row struct first, then the two bindings, then the reader, which names them.
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(13) sampler2D u_LocalShadowAtlas; // 13/14: freed by the cascade array (22/23 are the captures)
        ReadBuffer(14) LocalShadowViewsUB
        {
        	LocalShadowView localShadowViews[];
        };
        #include <Mesh/LocalLightShadow.glslh>

        // Heat ramp for the Light-Complexity debug view: 0 -> dark blue, up through cyan/green/yellow -> red.
        // Standard "jet"-style piecewise map so overlapping light volumes read as hotter pixels.
        vec3 HeatColor(float t)
        {
        	t = clamp(t, 0.0, 1.0);
        	return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
        	                  1.5 - abs(4.0 * t - 2.0),
        	                  1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
        }

        void main()
        {
        	vec4 ga = texture(u_GBufferA, v_TexCoord);
        	vec4 gb = texture(u_GBufferB, v_TexCoord);

        	vec3  albedo    = ga.rgb;
        	float metallic  = ga.a;
        	vec3  normal    = gb.rgb;
        	float roughness = max(gb.a, 0.04);
        	vec3  worldPos  = ReconstructWorldPosition(v_TexCoord, texture(u_GBufferDepth, v_TexCoord).r,
        	                                           u_InvJitteredViewProjection);

        	const int dbg = int(u_Params.x + 0.5);

        	// No geometry here (zero normal — the G-buffer is cleared to 0): discard so the LOADed forward scene
        	// (real procedural sky / skybox + grid) shows through.
        	const bool hasGeometry = dot(normal, normal) > 0.001;
        	if (!hasGeometry)
        	{
        		discard;
        	}

        	const vec4          ge           = texture(u_GBufferEmissive, v_TexCoord);
        	const uvec2         word         = texelFetch(u_GBufferShadingWord, ivec2(gl_FragCoord.xy), 0).rg;
        	const int           shadingModel = DesertShadingModelIndex(word);
        	const DesertPayload payload      = DesertUnpackPayload(word);

        	// Ambient occlusion as UE composes it for the ambient: the MATERIAL's (GBuffer AO, from the surface's
        	// AmbientOcclusion output — the forward path's whole AO) times the screen-space one (GTAO's visibility).
        	// z = GTAO ran this frame; its bent normal is then the direction the sky's irradiance is read along.
        	const bool  gtao  = u_Params.z > 0.5;
        	const vec4  ssao  = gtao ? textureLod(u_SSAO, v_TexCoord, 0.0) : vec4(0.5, 0.5, 0.5, 1.0);
        	float       ao    = ge.a * ssao.a;

        	// --- Debug channels (shown on the geometry, over the sky) ---
        	if (dbg == 1) { oColor = vec4(albedo, 1.0); return; }                          // Albedo
        	if (dbg == 2) { oColor = vec4(normalize(normal) * 0.5 + 0.5, 1.0); return; }   // Normal
        	if (dbg == 3) { oColor = vec4(vec3(metallic),  1.0); return; }                 // Metallic
        	if (dbg == 4) { oColor = vec4(vec3(roughness), 1.0); return; }                 // Roughness
        	if (dbg == 5) { oColor = vec4(vec3(ao),        1.0); return; }                 // Ambient Occlusion

        	// Light Complexity: count how many point/spot light VOLUMES cover this pixel (by radius/range +
        	// cone), then map the count to a heat colour. Cheap in deferred — the light data is already bound.
        	if (dbg == 7)
        	{
        		uint lights = 0u;
        		for (uint i = 0u; i < lightsMetadata.PointLightCount; i++)
        			if (distance(pointLights[i].position, worldPos) < pointLights[i].radius)
        				lights++;
        		for (uint i = 0u; i < lightsMetadata.SpotLightCount; i++)
        		{
        			vec3  toL = spotLights[i].position - worldPos;
        			if (length(toL) < spotLights[i].range && SpotConeFactor(spotLights[i], normalize(toL)) > 0.0)
        				lights++;
        		}
        		// Normalize by a fixed budget (8 overlapping lights = full red) so the scale is stable.
        		oColor = vec4(HeatColor(float(lights) / 8.0), 1.0);
        		return;
        	}

        	// Material Complexity: the G-buffer pass stashed the material's sampled-texture count in the shading word's TEXTURES field
        	// (0..3). Heat-map it as a proxy for per-pixel shading cost (UE-style shader/material complexity).
        	if (dbg == 9) { oColor = vec4(HeatColor(DesertSampledTextureCount(word) / 3.0), 1.0); return; }

        	// Shading Model / Sun Shadow Receive: the word's other two fields, decoded from the same integer fetch (the
        	// slot is RG32_UINT; a float view of it would reinterpret the bits). One flat colour per model index (golden-
        	// ratio hue walk, so neighbouring indices differ); green = receives the sun's cascades, red = opted out.
        	if (dbg == 10)
        	{
        		const float hue = fract(float(shadingModel) * 0.618034);
        		oColor          = vec4(clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0), 1.0);
        		return;
        	}
        	if (dbg == 11) { oColor = DesertReceivesSunShadows(word) ? vec4(0.1, 0.9, 0.2, 1.0) : vec4(0.9, 0.1, 0.1, 1.0); return; }

        	// --- Lit: shadow-mapped directional sun (N·L) + full lit point/spot lights ---
        	vec3 N    = normalize(normal);
        	vec3 view = normalize(u_CameraPos.xyz - worldPos);

        	// Directional sun (energy-normalized lit), occluded by the cascaded shadow map.
        	// TWO OCCLUDERS OF ONE SUN, multiplied: the cascaded maps for opaque geometry and the cloud
        	// layer's own volumetric transmittance. Only the DIRECTIONAL term is attenuated — the ambient
        	// below is the whole sky dome, which a cloud deck occludes with a different geometry
        	// (a hemisphere, not a direction) and which UE answers with a separate Sky AO volume. Folding
        	// a directional occlusion into an omnidirectional term would darken the shaded side of every
        	// object under a cloud by an amount nothing in the world justifies.
        	vec3  L        = normalize(-u_LightDir.xyz);
        	// `cascade` is the chosen cascade index, which this pass has no debug view for — the forward
        	// mesh shaders tint by it under Scene Settings -> Debug -> Cascades and the deferred composite's
        	// debug modes are a different, G-buffer-driven set. Read and dropped rather than given a second
        	// ShadowFactor overload, because a second overload is how the four copies started.
        	int   cascade  = -1;
        	// The cascades skip a surface whose material does not receive them (the shading word's NO_SUN_SHADOWS bit), then the
        	// cloud layer — the cloud factor AFTER the toggle, exactly as the forward pass assembles it.
        	float shadow   = DesertReceivesSunShadows(word) ? ShadowFactor(worldPos, N, L, cascade) : 1.0;
        	shadow        *= CloudShadowFactor(worldPos);
        	vec3  radiance = u_LightColor.rgb * u_LightColor.a;

        	const DesertSurface surface = DesertMakeSurface(worldPos, N, view, albedo, metallic, roughness);

        	vec3 result = DesertEvaluateShadingModel(shadingModel, DesertSunLight(u_LightDir.xyz, radiance, shadow),
        	                                         surface, payload);

        	// Point lights (the city payoff): every source contributes full Cook-Torrance lit, occluded by its cube of
        	// atlas views when it casts shadows.
        	for (uint i = 0u; i < lightsMetadata.PointLightCount; i++)
        		result += DesertEvaluateShadingModel(shadingModel, DesertShadowedPointLightAt(pointLights[i], worldPos, N),
        		                                     surface, payload);

        	// Spot lights (street lamps / headlights), occluded by their one perspective atlas view.
        	for (uint i = 0u; i < lightsMetadata.SpotLightCount; i++)
        		result += DesertEvaluateShadingModel(shadingModel, DesertShadowedSpotLightAt(spotLights[i], worldPos, N),
        		                                     surface, payload);

        	// One-bounce GI: the denoised SSGI or RSM-GI buffer (System.Black when GI is off) - already denoised, a
        	// second, depth-blind blur here would bleed across edges.
        	const vec3 indirect = textureLod(u_GI, v_TexCoord, 0.0).rgb;

        	if (dbg == 6) { oColor = vec4(indirect, 1.0); return; } // Indirect GI only

        	// Ambient = the BAKED ENVIRONMENT (split-sum IBL: diffuse irradiance + prefiltered specular)
        	// plus the one-bounce indirect, attenuated by SSAO — assembled by the SAME shared function the
        	// forward mesh shaders call, so a scene rendered through either RenderingPath gets one ambient
        	// model. The only argument that differs there is `indirect`, which the forward path passes as
        	// zero because it has no bounce gather.
        	// The specular radiance along the reflection is the traced one where SSR answered, the reflection
        	// environment (captures, then sky) for the rest (UE ReflectionEnvironmentPixelShader: SSR over the
        	// captures, then the split-sum Fresnel of both together).
        	DesertAmbient sampled = DesertSampleAmbient(surface, indirect, ao);
        	// The sky's irradiance along GTAO's bent normal - the mean unoccluded direction - instead of the surface
        	// normal (Jimenez 2016 §5: a crease is lit by the sky it sees, not by the sky its normal points at).
        	if (gtao && dot(ssao.rgb, ssao.rgb) > 0.0)
        	{
        		const vec3 bent = ssao.rgb * 2.0 - 1.0;
        		if (dot(bent, bent) > 1e-4)
        			sampled.Irradiance = DesertAmbientIrradiance(normalize(bent));
        	}
        	if (u_CameraPos.w > 0.5)
        		sampled.Prefiltered = DesertComposeTracedReflection(textureLod(u_SSR, v_TexCoord, 0.0), sampled.Prefiltered);
        	vec3 ambient = DesertEvaluateShadingModelAmbient(shadingModel, sampled, surface, payload);

        	// Self-illumination (view-independent) — added here (not lit) so HDR emissive reaches the composite
        	// and blooms, matching the forward path. GBufferEmissive is 0 where the material has none.
        	vec3 emissive = ge.rgb;

        	oColor = vec4(result + ambient + emissive, 1.0);
        }
    }
}

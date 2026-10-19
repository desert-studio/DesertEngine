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

        // DESERT_MEGALIGHTS (Shadows Stochastic, DeferredLightingRenderer's MegaLights program): the local lights
        // arrive as MegaLights' denoised lighting (u_MegaLightsDiffuse / u_MegaLightsSpecular below), so this program reads neither the
        // light rows nor their shadow maps.
        #ifndef DESERT_MEGALIGHTS
        #include <Mesh/PointLight.glslh>      // binding 6  (SSBO PointLightsUB) + BRDF
        #include <Mesh/Spotlight.glslh>       // binding 16 (SSBO SpotLightsUB)
        #endif
        #include <Mesh/LightsMetadata.glslh>  // binding 4  (UB LightsMetadata: point/spot/dir counts)
        // THE direct-light BRDF, shared with the forward mesh shaders and with the point/spot headers
        // above (which already include it). Named explicitly because this pass calls it directly.
        #include <Mesh/BRDF.glslh>            // DirectLighting needs it first; under DESERT_MEGALIGHTS no light header brings it
        #include <Mesh/DirectLighting.glslh>
        // The one-bounce estimate GatherIndirectGI below is built from — direct light seen a second time,
        // so it is the SAME BRDF, reached through the same text. Must follow DirectLighting.glslh, which it
        // calls.
        #include <Mesh/IndirectBounce.glslh>
        // World position from the G-buffer depth (u_GBufferDepth below): the one reconstruction every deferred
        // reader shares.
        #include <Common/ReconstructPosition.glslh>

        In(0) vec2 v_TexCoord;

        // SHARED SAMPLER STATES (UE's shared samplers). Every input of this pass is a SEPARATE texture read
        // through one of these three, bound once by the pass (DeferredLightingRenderer::DeclareCompositeBindings).
        // A combined sampler per input put 17 samplers in this fragment stage, over MoltenVK's
        // maxPerStageDescriptorSamplers of 16, and the device refused the program; now the stage holds 3.
        // Numbered by the program build after the last slot of set 0 (SharedSamplerBinding.hpp); 28 up is the
        // shader graph's window, and set 0 ends at 24 (u_SSR), so they land on 25..27.
        //
        // Each input keeps the name the C++ binds and the shared includes read (Mesh/CascadedShadow.glslh,
        // Mesh/AmbientIBL.glslh, ...): right after its declaration a macro of the SAME name turns every later use
        // into the combined form `sampler2D(texture, sampler)`. The preprocessor never re-expands a macro inside
        // its own expansion, so the inner name is the texture itself.
        Uniform sampler u_LinearRepeatSampler;
        Uniform sampler u_LinearClampSampler;
        Uniform sampler u_PointClampSampler;

        // The G-buffer's layout and the shading-model ids it carries (the shading word, Emissive.a).
        // The shading word (ShadingModels/ShadingModelContract.glslh): an RG32_UINT target, read ONLY by texelFetch.
        layout(set = 0, binding = 1) uniform utexture2D u_GBufferShadingWord;
        #define u_GBufferShadingWord usampler2D(u_GBufferShadingWord, u_PointClampSampler)
        // Device depth (nearest sampler: DEPTH32F is not filtered); world position = ReconstructWorldPosition.
        layout(set = 0, binding = 21) uniform texture2D u_GBufferDepth;
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        layout(set = 0, binding = 2) uniform texture2D u_GBufferA; // rgb = albedo, a = metallic
        #define u_GBufferA sampler2D(u_GBufferA, u_LinearRepeatSampler)
        layout(set = 0, binding = 3) uniform texture2D u_GBufferB; // rgb = world normal, a = roughness
        #define u_GBufferB sampler2D(u_GBufferB, u_LinearRepeatSampler)
        layout(set = 0, binding = 8) uniform texture2D u_SSAO; // r = ambient-occlusion factor (1 = lit)
        #define u_SSAO sampler2D(u_SSAO, u_LinearRepeatSampler)
        layout(set = 0, binding = 9) uniform texture2D u_GBufferEmissive; // rgb = HDR emissive, a = material AO
        #define u_GBufferEmissive sampler2D(u_GBufferEmissive, u_LinearRepeatSampler)

        // THE BAKED ENVIRONMENT — the same trio the forward mesh shaders bind, and the whole point of
        // this pass having an ambient term at all. Until 2026-09-03 the deferred composite read NEITHER
        // cube and floored its ambient at a flat vec3(0.08), so 51 of the repository's 53 scenes lit
        // their ordinary static opaque geometry from a constant while the skinned meshes, the
        // custom-shader meshes and the glass drawn forward over this very composite lit themselves from
        // the sky. One material was shaded two ways depending on whether its mesh had a skeleton.
        //
        // New slots (17..19) rather than the forward path's 8..10: those three are already taken here by
        // u_SSAO / u_GBufferEmissive (and 10, once the RSM GI buffer), and this pass shares no descriptor layout with the mesh
        // shaders — it is a fullscreen triangle with its own material. Only the NAMES have to match, because
        // the material binds by name.
        layout(set = 0, binding = 17) uniform textureCube u_EnvIrradianceTex; // diffuse irradiance (cosine-convolved sky)
        #define u_EnvIrradianceTex samplerCube(u_EnvIrradianceTex, u_LinearClampSampler)
        layout(set = 0, binding = 18) uniform textureCube u_EnvSpecularTex; // GGX-prefiltered radiance, roughness across mips
        #define u_EnvSpecularTex samplerCube(u_EnvSpecularTex, u_LinearClampSampler)
        layout(set = 0, binding = 19) uniform texture2D u_BRDFLUTTexture; // split-sum BRDF integration (cosLo, roughness)
        #define u_BRDFLUTTexture sampler2D(u_BRDFLUTTexture, u_LinearClampSampler)
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). Slot 20: the
        // first free one in this layout.
        Uniform(20) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;

        // THE TRACED GI'S FINAL GATHER (u_Params.w == 2): per pixel, the irradiance (/ pi) and the specular
        // radiance of the screen probes (Graphic/GlobalIllumination/ScreenProbeGather, SoftwareTraced) or of the
        // radiance cache read in place (IrradianceFieldGather, ProbeVolume), in place of the sky's two cubes —
        // the rays saw the sky only where it is visible. Black in the other GI modes (unread).
        Uniform(30) texture2D u_GIDiffuse;
        #define u_GIDiffuse sampler2D(u_GIDiffuse, u_PointClampSampler)
        Uniform(31) texture2D u_GISpecular;
        #define u_GISpecular sampler2D(u_GISpecular, u_PointClampSampler)

        Out(0) vec4 oColor;

        const vec3 Fdielectric = vec3(0.04); // base reflectance for dielectrics (matches Lit.glsl.frag)

        // The ambient model itself, shared verbatim with StaticMeshLit / StaticMeshLit_Instanced /
        // SkinnedMeshLit. Included after the three bindings above because it names them.
        // The reflection captures (Common/ReflectionEnvironment.glslh): the per-capture records and their cube array,
        // bound by the Composite node as scene view inputs (SceneViewInputs::ReflectionCaptures / ...Cubes).
        // SET 1, bindings 0 and 1, so the set-0 table (pinned by name in ShaderCacheKey) keeps its free slots for the
        // passes that come next. 25-34 are the reserved scene-read range (Core::IsSceneReadBinding), 35-40 the
        // reserved particle-read range (Core::IsParticleReadBinding), 41-45 the reserved virtual-shadow-raster range
        // (Core::kReservedBindingRanges, the slot table) and 46 up is the window a shader graph's own
        // resources are numbered into
        // (Core::kGraphOwnedBindingFirst) - which no engine declaration may enter, graph or no graph in this
        // program. A second set is outside both reservations by construction (they are set-0 numbers), and the
        // pass binds by name: the backend resolves each name to its (set, binding) from the reflection.
        // Raw layout() for the buffer: the DSL's ReadBuffer(n) sugar has no set form.
        #define DESERT_REFLECTION_CAPTURES
        #include <Common/ReflectionCaptureGpu.glslh>
        layout(std430, set = 1, binding = 0) readonly buffer ReflectionCapturesUB
        {
            uvec4                u_ReflectionCaptureHeader; // x = capture count
            ReflectionCaptureGpu u_ReflectionCaptures[];
        };
        layout(set = 1, binding = 1) uniform textureCubeArray u_ReflectionCaptureCubes;
        #define u_ReflectionCaptureCubes samplerCubeArray(u_ReflectionCaptureCubes, u_LinearClampSampler)
        // The screen-space reflection of this frame (SSRRenderer's resolved trace, full resolution): rgb = the traced
        // radiance, a = the share of the specular lobe the trace answered. When SSR is off it is System.Black (opaque)
        // and u_CameraPos.w = 0: nothing is read, the environment answers the whole lobe.
        layout(set = 0, binding = 24) uniform texture2D u_SSR;
        #define u_SSR sampler2D(u_SSR, u_LinearClampSampler)
        // u_CameraPos.w: what u_SSR holds (SceneRenderer::AddFrameComposite).
        #define REFLECTION_INPUT_NONE   0
        #define REFLECTION_INPUT_SCREEN 1 // ReflectionMethod::ScreenSpace (UE bLumenReflectionInputIsSSR)
        #define REFLECTION_INPUT_TRACED 2 // SoftwareTraced / HardwareTraced (UE Lumen reflections)
        #include <Common/SSRGate.glslh>
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
        	vec4 u_Params;     // x = debug mode (0..9), y = GI intensity (0 = off), z = SSAO enabled, w = GI mode
        	vec4 u_CameraPos;  // xyz = camera world position (for the view vector); w = REFLECTION_INPUT_* of u_SSR
        	mat4 u_InvJitteredViewProjection; // the inverse of the matrix the G-buffer depth was rasterised with
        	mat4 u_JitteredViewProjection;    // the matrix itself: the contact shadows march the depth in its space
        	vec4 u_ScreenSpaceGI;  // x = the screen-space gather's samples per pixel (Scalability
        	                       // GlobalIllumination.ScreenSpaceSamples); yzw unused
        };

        float ssgiHash(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // One-bounce screen-space GI: gather nearby G-buffer texels, work out how much of the SUN each of
        // them reflects TOWARD this surface, and accumulate it. Screen-space only (no extra passes / no
        // lit-colour feedback); misses off-screen and point-lit bounces.
        //
        // HOW LITTLE THIS DELIVERS IN A ROOM, measured on CornellDemo (the scene that existed to show
        // colour bleed) at --camera 0,300,1400, 715x764, noise floor 0 pixels:
        //   * the whole feature on vs off:  mean 0.11/255, max 16/255, 13.4 % of pixels;
        //   * the isolated indirect buffer: 0.000 in every statistic on the floor, near the walls and at
        //     the centre alike, with a faint band only where a SUNLIT wall meets the floor;
        //   * aiming the sun at the unlit wall: no change whatsoever.
        // Three divisors stack. A neighbour the sun does not reach returns exactly vec3(0) from the
        // shared BRDF; BounceFalloff is 1 + d²/(1 m)², which is 9.4 across a 290 cm room; and the sum is
        // divided by SAMPLES whether or not a sample found an emitter, so WIDENING the radius makes the
        // bleed weaker rather than longer-ranged — measured at 0.45 instead of 0.12, the one non-zero
        // floor reading fell from 0.001 to 0.000. RADIUS is therefore not the knob it looks like.
        //
        // What would change the answer is bouncing the point and spot lights as well as the sun, at a
        // price of SAMPLES × lights BRDF evaluations per pixel inside the pass that produces the lit
        // colour. That is a design decision for the owner, not a constant for whoever is passing.
        //
        // The per-sample estimate is Mesh/IndirectBounce.glslh, which shades the bouncing neighbour through the
        // SAME Mesh/DirectLighting.glslh the sun below is shaded with. It used to shade it here, inline, as
        // `nAlb * max(0.0, dot(nN, sunL)) * sunRadiance` — the un-normalized Lambert this engine deleted from
        // its forward mesh shaders — which made every emitter PI times brighter than the same surface is when
        // the main pass shades it. The G-buffer already carries the neighbour's metallic (GBufferA.a) and
        // roughness (GBufferB.a), so nothing extra is sampled to shade it properly.
        //
        // WHAT THE ESTIMATE DELIBERATELY DOES NOT MODEL, stated so the omission is a property and not an
        // oversight: the neighbour's own occlusion. The sun below is attenuated by ShadowFactor *
        // CloudShadowFactor; re-deriving that per gather sample means a cascade choice plus nine PCF taps
        // TWELVE more times per pixel, and there is no lit-colour buffer to read it back from because this
        // gather runs inside the very pass that produces the lit colour. A neighbour standing in shadow
        // therefore bounces as though it stood in sun.
        vec3 GatherIndirectGI(vec2 uv, vec3 worldPos, vec3 N, vec3 sunL, vec3 sunRadiance)
        {
        	const int   SAMPLES = int(u_ScreenSpaceGI.x + 0.5); // the machine's quality, never the scene's
        	// NOT a range knob, measured: because the sum below is divided by SAMPLES whether or not a
        	// sample found an emitter, and BounceFalloff grows with distance, a WIDER radius spreads the
        	// same fixed budget over dimmer contributions. At 0.45 instead of 0.12 the only non-zero
        	// reading on CornellDemo's floor fell from 0.001 to 0.000. The line used to say "wider =
        	// longer-range bleed, but noisier"; the first half of that is false.
        	const float RADIUS  = 0.12;   // screen-space gather radius (UV)
        	const float GOLDEN  = 2.3999632; // golden angle for an even spiral

        	float ang      = ssgiHash(uv * 2048.0) * 6.2831853;
        	vec3  indirect = vec3(0.0);

        	for (int i = 0; i < SAMPLES; i++)
        	{
        		float r = RADIUS * sqrt((float(i) + 0.5) / float(SAMPLES));
        		float a = ang + float(i) * GOLDEN;
        		vec2  suv = uv + vec2(cos(a), sin(a)) * r;
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        		vec4 nb = texture(u_GBufferB, suv);
        		if (dot(nb.rgb, nb.rgb) <= 0.001) continue; // sky texel -> no bounce

        		vec4  na   = texture(u_GBufferA, suv);
        		vec3  nPos = ReconstructWorldPosition(suv, texture(u_GBufferDepth, suv).r, u_InvJitteredViewProjection);
        		vec3  nF0  = mix(Fdielectric, na.rgb, na.a);

        		indirect += EvaluateBounceSample(worldPos, N, nPos, normalize(nb.rgb), na.rgb, nF0, na.a,
        		                                 max(nb.a, 0.04), sunL, sunRadiance);
        	}
        	return indirect / float(SAMPLES);
        }

        // The cascaded shadow's two bindings, at slots free in THIS layout. The text that reads them is
        // one file for every shader in the engine that shades a surface with the sun —
        // Mesh/CascadedShadow.glslh, included right below because it names what is declared here.
        // DESERT_SUN_SHADOW_SCREEN_MASK (DeferredLightingRenderer's second program, "Shadows: Ray Traced Sun"): the
        // sun's visibility is the denoised screen mask, and the cascades are compiled out.
        #ifdef DESERT_SUN_SHADOW_SCREEN_MASK
        Uniform(5) texture2D u_ScreenShadowMask; // r = visibility, 1 = lit (render extent = this target's)
        #define u_ScreenShadowMask sampler2D(u_ScreenShadowMask, u_PointClampSampler)
        #else
        // The four cascades are ONE array texture (layer c = cascade c), read through the shared repeat state.
        Uniform(5) texture2DArray u_ShadowCascades;
        #define u_ShadowCascades sampler2DArray(u_ShadowCascades, u_LinearRepeatSampler)
        #endif
        Uniform(7) ShadowUB {
        	mat4 u_LightViewProj[4];
        	vec4 u_ShadowParams;      // x = tan(sun source angle / 2), y = enabled (>0.5), z = debug mode (0/1/2), w = cascade count
        	vec4 u_DebugParams;       // x = show normals (>0.5), y = light debug (>0.5); z,w reserved
        	vec4 u_CascadeTexelWorld; // per-cascade world size of one shadow-map texel (x..w = cascade 0..3)
        	vec4 u_CascadeDepthRange; // per-cascade world length the [0,1] map depth spans (x..w = cascade 0..3)
        	vec4 u_ContactShadowView; // x = contact shadow ray steps (0 = off), y = tan(fovY / 2), z = dither frame 0..7
        	vec4 u_SunContactShadow;  // x = the sun's ContactShadowLength, y = in world space (>0.5),
        	                          // z = its Contact Shadow Casting Intensity, w = Non Casting Intensity
        	vec4 u_ShadowFilter;      // x = PCSS blocker search taps, y = filter taps (sun PCSS + local lights PCF)
        };

        #ifdef DESERT_SUN_SHADOW_SCREEN_MASK
        #define DESERT_SUN_SHADOW(worldPos, N, L, cascade) texelFetch(u_ScreenShadowMask, ivec2(gl_FragCoord.xy), 0).r
        #else
        #include <Mesh/CascadedShadow.glslh>
        #define DESERT_SUN_SHADOW(worldPos, N, L, cascade) ShadowFactor(worldPos, N, L, cascade)
        #endif

        // THE CONTACT SHADOWS (UE ContactShadowLength): a short screen-space ray toward each light through
        // u_GBufferDepth. Included after ShadowUB, which carries the march's view values; Mesh/LocalLightShadow.glslh
        // below applies it to the point and spot lights, the sun's is applied in main().
        #define CONTACT_SHADOW_DEPTH u_GBufferDepth
        #define CONTACT_SHADOW_VP    u_JitteredViewProjection
        // The hit texel's NO_CONTACT_SHADOW bit (UE CastContactShadow( GBuffer )): which intensity occludes.
        #define CONTACT_SHADOW_SHADING_WORD u_GBufferShadingWord
        #include <Common/ContactShadow.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — a second, independent occluder of the same sun.
        // Declared here, evaluated by the shared receiver text below: the wrapper used to live in this
        // file and this file ALONE, which is exactly why no forward-shaded surface in the engine ever
        // received a cloud shadow. See Common/CloudShadowReceiver.glslh.
        layout(set = 0, binding = 11) uniform texture2D u_CloudShadowMap;
        #define u_CloudShadowMap sampler2D(u_CloudShadowMap, u_LinearRepeatSampler)
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
        #ifndef DESERT_MEGALIGHTS
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(13) texture2D u_LocalShadowAtlas; // 13/14: freed by the cascade array
        #define u_LocalShadowAtlas sampler2D(u_LocalShadowAtlas, u_PointClampSampler)
        ReadBuffer(14) LocalShadowViewsUB
        {
        	LocalShadowView localShadowViews[];
        };
        #include <Mesh/LocalLightShadow.glslh>
        #else
        // MegaLights' local lighting (System::MegaLightsRenderer: ReSTIR DI over the light grid, ray-query visibility,
        // the screen-space denoiser), as two demodulated signals: diffuse * MegaLightsDiffuseReflectance plus
        // specular * MegaLightsSpecularReflectance of this texel is its light.
        Uniform(32) texture2D u_MegaLightsDiffuse;
        #define u_MegaLightsDiffuse sampler2D(u_MegaLightsDiffuse, u_PointClampSampler)
        Uniform(33) texture2D u_MegaLightsSpecular;
        #define u_MegaLightsSpecular sampler2D(u_MegaLightsSpecular, u_PointClampSampler)
        #include <Mesh/MegaLightsDemodulation.glslh>
        #endif

        // THE VIEW'S LIGHT GRID ("LightGrid", Programs/Deferred/LightGridBuild.shader): a pixel is shaded by
        // the point and spot lights of its froxel only (UE LightGrid). The parameters, then the cells, at set-0
        // slots 22/23 (freed by the cascade array; 28 up is the shader graph's window, which this pass may not enter).
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(22) LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        ReadBuffer(23) LightGridCellsUB
        {
        	uint lightGridCells[];
        };

        #ifndef DESERT_MEGALIGHTS
        // One light of the grid's index space (Mesh/LightGrid.glslh): points first, then spots.
        vec3 ShadeGridLight(uint light, int shadingModel, vec3 worldPos, vec3 N, float subsurfaceOpacity,
                            DesertSurface surface, DesertPayload payload)
        {
        	if (light < lightsMetadata.PointLightCount)
        		return DesertEvaluateShadingModel(shadingModel,
        		                                  DesertShadowedPointLightAt(pointLights[light], worldPos, N, subsurfaceOpacity),
        		                                  surface, payload);
        	return DesertEvaluateShadingModel(shadingModel,
        	                                  DesertShadowedSpotLightAt(spotLights[light - lightsMetadata.PointLightCount],
        	                                                            worldPos, N, subsurfaceOpacity),
        	                                  surface, payload);
        }
        #endif

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
        	// AmbientOcclusion output — the forward path's whole AO) times the screen-space one. z = SSAO enabled.
        	const float ssao = (u_Params.z > 0.5) ? texture(u_SSAO, v_TexCoord).r : 1.0;
        	float       ao   = ge.a * ssao;

        	// --- Debug channels (shown on the geometry, over the sky) ---
        	if (dbg == 1) { oColor = vec4(albedo, 1.0); return; }                          // Albedo
        	if (dbg == 2) { oColor = vec4(normalize(normal) * 0.5 + 0.5, 1.0); return; }   // Normal
        	if (dbg == 3) { oColor = vec4(vec3(metallic),  1.0); return; }                 // Metallic
        	if (dbg == 4) { oColor = vec4(vec3(roughness), 1.0); return; }                 // Roughness
        	if (dbg == 5) { oColor = vec4(vec3(ao),        1.0); return; }                 // Ambient Occlusion

        	// Light Complexity: the number of point/spot lights the lighting below shades this pixel with — its froxel's
        	// count in the view's light grid (UE's clustered "light count" view), so the picture is the cost actually
        	// paid, not a separate estimate. An overflowed cell is shaded by every light and shows that total.
        	// Normalized by a fixed budget (8 lights = full red) so the scale is stable.
        	if (dbg == 7)
        	{
        		const uint cell   = LightGridCellAt(lightGrid, gl_FragCoord.xy, worldPos);
        		const uint count  = lightGridCells[cell];
        		const uint lights = LightGridCellOverflowed(lightGrid, count)
        		                        ? lightsMetadata.PointLightCount + lightsMetadata.SpotLightCount
        		                        : count;
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
        	// The sun's contact shadow joins its cascades: it is the near-contact part of the same occlusion.
        	// The receiver's subsurface opacity (UE IsSubsurfaceModel + CustomData.a; -1 = not subsurface): its
        	// contact shadows fade with the hit distance.
        	const float subsurfaceOpacity = DesertSubsurfaceOpacity(shadingModel, payload);
        	float shadow   = DesertReceivesSunShadows(word)
        	                     ? DESERT_SUN_SHADOW(worldPos, N, L, cascade) *
        	                           ContactShadowFactor(worldPos, N, L, u_SunContactShadow, subsurfaceOpacity)
        	                     : 1.0;
        	// The relief's own shadow (parallax occlusion, the word's SUN_SELF_SHADOW field): geometry, always applied.
        	shadow        *= DesertSunSelfShadow(word);
        	shadow        *= CloudShadowFactor(worldPos);
        	vec3  radiance = u_LightColor.rgb * u_LightColor.a;

        	const DesertSurface surface = DesertMakeSurface(worldPos, N, view, albedo, metallic, roughness);

        	vec3 result = DesertEvaluateShadingModel(shadingModel, DesertSunLight(u_LightDir.xyz, radiance, shadow),
        	                                         surface, payload);

        	// Point and spot lights (the city payoff): full Cook-Torrance lit, each occluded by its atlas views when it
        	// casts shadows — the lights of this pixel's froxel only, in the order the full loop would sum them. An
        	// overflowed cell (more lights than its list holds) is shaded by every light: slower, never wrong.
        	// Under MegaLights the same lights arrive sampled, traced and denoised: the texel remodulated by the
        	// reflectances they were demodulated with ("MegaLights: Shade Samples"), diffuse and specular apart.
        #ifdef DESERT_MEGALIGHTS
        	result += texelFetch(u_MegaLightsDiffuse, ivec2(gl_FragCoord.xy), 0).rgb *
        	              MegaLightsDiffuseReflectance(albedo, metallic) +
        	          texelFetch(u_MegaLightsSpecular, ivec2(gl_FragCoord.xy), 0).rgb *
        	              MegaLightsSpecularReflectance(albedo, metallic, roughness, N, view);
        #else
        	const uint cell  = LightGridCellAt(lightGrid, gl_FragCoord.xy, worldPos);
        	const uint count = lightGridCells[cell];
        	if (!LightGridCellOverflowed(lightGrid, count))
        	{
        		const uint list = LightGridListOffset(lightGrid, cell);
        		for (uint j = 0u; j < count; j++)
        			result += ShadeGridLight(lightGridCells[list + j], shadingModel, worldPos, N, subsurfaceOpacity, surface,
        			                         payload);
        	}
        	else
        	{
        		const uint lights = lightsMetadata.PointLightCount + lightsMetadata.SpotLightCount;
        		for (uint i = 0u; i < lights; i++)
        			result += ShadeGridLight(i, shadingModel, worldPos, N, subsurfaceOpacity, surface, payload);
        	}
        #endif

        	// One-bounce screen-space GI (D6, u_Params.w == 1): gather from sun-lit G-buffer neighbours. Cheap and
        	// self-contained, but only geometry currently ON SCREEN can bounce, and there is no denoiser.
        	vec3  indirect    = vec3(0.0);
        	float giIntensity = u_Params.y;
        	int   giMode      = int(u_Params.w + 0.5);
        	if (giMode == 1 && giIntensity > 0.0)
        	{
        		indirect = GatherIndirectGI(v_TexCoord, worldPos, N, L, radiance) * giIntensity;
        	}

        	if (dbg == 6) { oColor = vec4(indirect, 1.0); return; } // Indirect GI only

        	// Ambient = the BAKED ENVIRONMENT (split-sum IBL: diffuse irradiance + prefiltered specular)
        	// plus the one-bounce indirect, attenuated by SSAO — assembled by the SAME shared function the
        	// forward mesh shaders call, so a scene rendered through either RenderingPath gets one ambient
        	// model. The only argument that differs there is `indirect`, which the forward path passes as
        	// zero because it has no bounce gather.
        	// The specular radiance along the reflection is the traced one where SSR answered, the indirect source's
        	// for the rest: the traced GI's final gather (screen probes or the irradiance field), else the reflection
        	// environment (captures, then sky) (UE ReflectionEnvironmentPixelShader: SSR over the rest, then the
        	// split-sum Fresnel of both together).
        	DesertAmbient sampled = DesertSampleAmbient(surface, indirect, ao);
        	// The reflection environment along the lobe (the captures, then the sky): what a traced ray's miss sees.
        	const vec3 environment     = sampled.Prefiltered;
        	const int  reflectionInput = int(u_CameraPos.w + 0.5);
        	if (giMode == 2)
        	{
        		// The traced final gather at this pixel (one texel each, the G-buffer's extent): the diffuse irradiance
        		// and the rough specular replace both sky terms, so a closed room neither glows nor mirrors the sky.
        		sampled.Irradiance  = texelFetch(u_GIDiffuse, ivec2(gl_FragCoord.xy), 0).rgb;
        		sampled.Prefiltered = texelFetch(u_GISpecular, ivec2(gl_FragCoord.xy), 0).rgb;
        	}
        	if (reflectionInput == REFLECTION_INPUT_TRACED)
        	{
        		// UE MixSpecularAndRoughReflections (DiffuseIndirectComposite.usf) for Lumen reflections: the traced
        		// reflection - its hits, its misses answered by the captures, then the sky (better than UE's sky
        		// alone indoors) - answers TracedReflectionsAlpha of the lobe, the traced GI's rough specular the
        		// rest. Without traced GI there is no rough specular: the traced reflection answers the whole lobe,
        		// its screen hits past the max roughness included, as ScreenSpace's do.
        		const vec3 traced = DesertComposeTracedReflection(textureLod(u_SSR, v_TexCoord, 0.0), environment);
        		sampled.Prefiltered = giMode == 2 ? mix(sampled.Prefiltered, traced, TracedReflectionsAlpha(gb.a)) : traced;
        	}
        	else if (reflectionInput == REFLECTION_INPUT_SCREEN)
        		// UE's SSR input: the screen's hits over the GI's specular (or the environment), by their coverage.
        		sampled.Prefiltered = DesertComposeTracedReflection(textureLod(u_SSR, v_TexCoord, 0.0), sampled.Prefiltered);
        	vec3 ambient = DesertEvaluateShadingModelAmbient(shadingModel, sampled, surface, payload);

        	// Self-illumination (view-independent) — added here (not lit) so HDR emissive reaches the composite
        	// and blooms, matching the forward path. GBufferEmissive is 0 where the material has none.
        	vec3 emissive = ge.rgb;

        	oColor = vec4(result + ambient + emissive, 1.0);
        }
    }
}

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
        // TWO PROGRAMS OF ONE TEXT, split as UE splits RenderLights from DiffuseIndirectComposite (GPU-TURN-3):
        //  - DESERT_DEFERRED_DIRECT, "Deferred: Direct Lighting": the sun, atmosphere light 1 and the local lights
        //    with their shadows, into an RGBA32F target (oDirect). It reads no GI, AO or reflection, so it has no
        //    edge to the async chain and runs on the graphics queue inside its window.
        //  - no define, "Deferred: Composite": the ambient (GI, reflections, AO), the emissive and the subsurface
        //    split over that direct sum, read back unrounded (RGBA32F) - the still frame is the same arithmetic.
        // The shadow-method defines below (DESERT_MEGALIGHTS, DESERT_SUN_SHADOW_SCREEN_MASK, ...) are the direct
        // program's permutations (DeferredLightingRenderer::ProgramVariant); the composite has one program.

        // DESERT_MEGALIGHTS (Shadows Stochastic, DeferredLightingRenderer's MegaLights program): the local lights
        // arrive as MegaLights' denoised lighting (MegaLightsDiffuseTexture / MegaLightsSpecularTexture below), so this program reads neither the
        // light rows nor their shadow maps.
        #if defined(DESERT_DEFERRED_DIRECT) && !defined(DESERT_MEGALIGHTS)
        #include <Mesh/PointLight.glslh>      // binding 6  (SSBO PointLightsUB) + BRDF
        #include <Mesh/Spotlight.glslh>       // binding 16 (SSBO SpotLightsUB)
        #endif
        #ifdef DESERT_DEFERRED_DIRECT
        #include <Mesh/LightsMetadata.glslh>  // binding 4  (UB LightsMetadata: point/spot/dir counts)
        #endif
        // THE direct-light BRDF, shared with the forward mesh shaders and with the point/spot headers
        // above (which already include it). Named explicitly because this pass calls it directly.
        #include <Mesh/BRDF.glslh>            // DirectLighting needs it first; under DESERT_MEGALIGHTS no light header brings it
        #include <Mesh/DirectLighting.glslh>
        // World position from the G-buffer depth (GBufferDepthTexture below): the one reconstruction every deferred
        // reader shares.
        #include <Common/ReconstructPosition.glslh>

        In(0) vec2 v_TexCoord;

        // SHARED SAMPLER STATES (UE's shared samplers). Every input of this pass is a SEPARATE texture read
        // through one of these three, bound once by the pass (DeferredLightingRenderer::DeclareCompositeBindings).
        // A combined sampler per input put 17 samplers in this fragment stage, over MoltenVK's
        // maxPerStageDescriptorSamplers of 16, and the device refused the program; now the stage holds 3.
        // Numbered by the program build after the last slot of set 0 (SharedSamplerBinding.hpp); 28 up is the
        // shader graph's window, and set 0 ends at 24 (SsrTexture), so they land on 25..27.
        //
        // Each input keeps the name the C++ binds and the shared includes read (Mesh/CascadedShadow.glslh,
        // Mesh/AmbientIBL.glslh, ...): right after its declaration a macro of the SAME name turns every later use
        // into the combined form `sampler2D(texture, sampler)`. The preprocessor never re-expands a macro inside
        // its own expansion, so the inner name is the texture itself.
        Uniform sampler LinearRepeatSampler;
        #ifndef DESERT_DEFERRED_DIRECT // the direct program samples nothing through it (no sky, BRDF table, SSR)
        Uniform sampler LinearClampSampler;
        #endif
        Uniform sampler PointClampSampler;

        // The G-buffer's layout and the shading-model ids it carries (the shading word, Emissive.a).
        // The shading word (ShadingModels/ShadingModelContract.glslh): an RG32_UINT target, read ONLY by texelFetch.
        layout(set = 0, binding = 1) uniform utexture2D GBufferShadingWordTexture;
        #define GBufferShadingWordTexture usampler2D(GBufferShadingWordTexture, PointClampSampler)
        // Device depth (nearest sampler: DEPTH32F is not filtered); world position = ReconstructWorldPosition.
        layout(set = 0, binding = 21) uniform texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        layout(set = 0, binding = 2) uniform texture2D GBufferATexture; // rgb = albedo, a = metallic
        #define GBufferATexture sampler2D(GBufferATexture, LinearRepeatSampler)
        layout(set = 0, binding = 3) uniform texture2D GBufferBTexture; // rgb = world normal, a = roughness
        #define GBufferBTexture sampler2D(GBufferBTexture, LinearRepeatSampler)
        #ifndef DESERT_DEFERRED_DIRECT // the composite's inputs: AO, emissive, sky, GI, reflections
        layout(set = 0, binding = 8) uniform texture2D SsaoTexture; // r = ambient-occlusion factor (1 = lit)
        #define SsaoTexture sampler2D(SsaoTexture, LinearRepeatSampler)
        layout(set = 0, binding = 9) uniform texture2D GBufferEmissiveTexture; // rgb = HDR emissive, a = material AO
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, LinearRepeatSampler)

        // THE BAKED ENVIRONMENT — the same trio the forward mesh shaders bind, and the whole point of
        // this pass having an ambient term at all. Until 2026-09-03 the deferred composite read NEITHER
        // cube and floored its ambient at a flat vec3(0.08), so 51 of the repository's 53 scenes lit
        // their ordinary static opaque geometry from a constant while the skinned meshes, the
        // custom-shader meshes and the glass drawn forward over this very composite lit themselves from
        // the sky. One material was shaded two ways depending on whether its mesh had a skeleton.
        //
        // New slots (17..19) rather than the forward path's 8..10: those three are already taken here by
        // SsaoTexture / GBufferEmissiveTexture (and 10, once the RSM GI buffer), and this pass shares no descriptor layout with the mesh
        // shaders — it is a fullscreen triangle with its own material. Only the NAMES have to match, because
        // the material binds by name.
        layout(set = 0, binding = 17) uniform textureCube EnvironmentIrradianceTexture; // diffuse irradiance (cosine-convolved sky)
        #define EnvironmentIrradianceTexture samplerCube(EnvironmentIrradianceTexture, LinearClampSampler)
        layout(set = 0, binding = 18) uniform textureCube EnvironmentSpecularTexture; // GGX-prefiltered radiance, roughness across mips
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        layout(set = 0, binding = 19) uniform texture2D BrdfLutTexture; // split-sum BRDF integration (cosLo, roughness)
        #define BrdfLutTexture sampler2D(BrdfLutTexture, LinearClampSampler)
        // The sky's look — how the two cubes above are read (Common/SkyLook.glslh). Slot 20: the
        // first free one in this layout.
        Uniform(20) SkyLookUB
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw) — Graphic::SkyLookGPU
            vec4 Gain;      // rgb = tint * intensity
        } skyLook;

        // THE TRACED GI'S FINAL GATHER (Params.w == 2, or 3 when it carries its own short-range AO): per pixel,
        // the irradiance (/ pi) and the specular
        // radiance of the screen probes (Graphic/GlobalIllumination/ScreenProbeGather, SoftwareTraced) or of the
        // radiance cache read in place (IrradianceFieldGather, ProbeVolume), in place of the sky's two cubes —
        // the rays saw the sky only where it is visible. Black in the other GI modes (unread).
        Uniform(30) texture2D GiDiffuseTexture;
        #define GiDiffuseTexture sampler2D(GiDiffuseTexture, PointClampSampler)
        Uniform(31) texture2D GiSpecularTexture;
        #define GiSpecularTexture sampler2D(GiSpecularTexture, PointClampSampler)
        // ...and the irradiance (/ pi) from behind the surface, -N, that a two-sided leaf lets through (UE
        // BackfaceDiffuseIndirect). Set 1 like the reflection captures below: the set-0 slots from 25 up are reserved,
        // and set 1's 0..6 are the captures and the view's radiance cache (Mesh/Surface/ViewIndirect.glslh).
        layout(set = 1, binding = 7) uniform texture2D GiBackfaceTexture;
        #define GiBackfaceTexture sampler2D(GiBackfaceTexture, PointClampSampler)

        Out(0) vec4 oColor;
        // SSS-PROFILE (CONTRACT §4): when the view scatters (CompositeView.z), a texel whose shading model is
        // screen-space scattered (the model's `SubsurfaceProfile` line) and whose material names a profile writes
        // its DEMODULATED diffuse (BaseColor = 1, UE AdjustBaseColorAndSpecularColorForSubsurfaceProfileLighting)
        // here, a = its profile row + 1, and only its specular + emissive to oColor; Graphic::SubsurfaceRenderer
        // scatters this and adds BaseColor x it back. Every other geometry texel writes 0 (a = 0: not scattered).
        // A storage image, not a 5th colour attachment: the scene target keeps its layout for every other pass
        // (UE has no explicit target at all - checkerboard or SceneColor.a, CONTRACT §2.1).
        layout(set = 1, binding = 8, rgba16f) writeonly uniform image2D SubsurfaceDiffuseTexture;

        const vec3 Fdielectric = vec3(0.04); // base reflectance for dielectrics (matches Lit.glsl.frag)

        // The ambient model itself, shared verbatim with StaticMeshLit / StaticMeshLit_Instanced /
        // StandardSurface's Vertex_Skinned cell. Included after the three bindings above because it names them.
        // The reflection captures (Common/ReflectionEnvironment.glslh): the per-capture records and their cube array,
        // bound by the Composite node as scene view inputs (SceneViewInputs::ReflectionCaptures / ...Cubes).
        // SET 1, bindings 0 and 1, so the set-0 table (pinned by name in ShaderCacheKey) keeps its free slots for the
        // passes that come next. 25-27 are the reserved scene-read range
        // (Core::IsSceneReadBinding) and 28 up is the window a shader graph's own resources are numbered into
        // (Core::kGraphOwnedBindingFirst) - which no engine declaration may enter, graph or no graph in this
        // program. A second set is outside both reservations by construction (they are set-0 numbers), and the
        // pass binds by name: the backend resolves each name to its (set, binding) from the reflection.
        // The declarations are the forward passes' (Mesh/Surface/ViewIndirect.glslh): the capture list and cubes
        // (set 1, b0-b1) and the view's world radiance cache (b2-b6), whose smooth read is the captures'
        // normalisation estimator wherever the view runs the cache (Mesh/AmbientIBL.glslh, REFL-NORM-SMOOTH).
        #include <Mesh/Surface/ViewIndirect.glslh>
        // The screen-space reflection of this frame (SSRRenderer's resolved trace, full resolution): rgb = the traced
        // radiance, a = the share of the specular lobe the trace answered. When SSR is off it is System.Black (opaque)
        // and CameraPosition.w = 0: nothing is read, the environment answers the whole lobe.
        layout(set = 0, binding = 24) uniform texture2D SsrTexture;
        #define SsrTexture sampler2D(SsrTexture, LinearClampSampler)
        // CameraPosition.w: what SsrTexture holds (SceneRenderer::AddFrameComposite).
        #define REFLECTION_INPUT_NONE   0
        #define REFLECTION_INPUT_SCREEN 1 // ReflectionMethod::ScreenSpace (UE bLumenReflectionInputIsSSR)
        #define REFLECTION_INPUT_TRACED 2 // SoftwareTraced / HardwareTraced (UE Lumen reflections)
        #include <Common/SSRGate.glslh>
        #include <Mesh/AmbientIBL.glslh>
        // The direct pass's answer ("Deferred: Direct Lighting", DeferredLightingRenderer::DeclareDirectBindings),
        // read at this texel: set 1 after the subsurface split.
        layout(set = 1, binding = 9) uniform texture2D DeferredDirectSpecularTexture;
        #define DeferredDirectSpecularTexture sampler2D(DeferredDirectSpecularTexture, PointClampSampler)
        layout(set = 1, binding = 10) uniform texture2D DeferredDirectLightingTexture;
        #define DeferredDirectLightingTexture sampler2D(DeferredDirectLightingTexture, PointClampSampler)
        #else
        // rgb = the direct sum (un-pre-exposed: a 100 000 lx sun is past RGBA16F, so the target is RGBA32F), or a
        // view this pass draws pre-exposed; a = kDeferredDirectShown for such a view, else the Shadow Cascades
        // view's tint index (0..3), else kDeferredDirectUntinted.
        Out(0) vec4 oDirect;
        // A scattered texel's direct specular (the composite's subsurface split); a 1x1 nothing reads otherwise.
        layout(set = 1, binding = 9, rgba32f) writeonly uniform image2D DeferredDirectSpecularImage;
        // The shading models' ambient bodies need the split-sum arithmetic only, never the sky's cubes.
        #define AMBIENT_IBL_NO_SAMPLERS
        #include <Mesh/AmbientIBL.glslh>
        #endif
        const float kDeferredDirectUntinted = -1.0;
        const float kDeferredDirectShown    = 4.0;
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
        	vec4 LightDirection;   // xyz = direction the light travels (away from the sun); w unused
        	vec4 LightColor; // rgb = colour, a = intensity
        	vec4 Params;     // x = debug mode (0..9), y unused, z = SSAO enabled, w = GI mode (0, 2, 3)
        	vec4 CameraPosition;  // xyz = camera world position (for the view vector); w = REFLECTION_INPUT_* of SsrTexture
        	mat4 InverseJitteredViewProjection; // the inverse of the matrix the G-buffer depth was rasterised with
        	mat4 JitteredWorldToClip;    // the matrix itself: the contact shadows march the depth in its space
        	vec4 CompositeView; // x unused; y = ViewFrame::PreExposure; z = 1 when the view scatters (SSS-PROFILE,
        	                    // SubsurfaceDiffuseTexture); w unused
        	vec4 Light1Direction; // atmosphere light 1 (UE Atmosphere Sun Light Index 1, the moon): xyz = travel
        	vec4 Light1Color;     // rgb = colour, a = intensity (after the atmosphere's transmittance); a == 0: none
        };

        #ifdef DESERT_DEFERRED_DIRECT // the direct program: the shadows, the cloud shadow, the local lights
        // The cascaded shadow's two bindings, at slots free in THIS layout. The text that reads them is
        // one file for every shader in the engine that shades a surface with the sun —
        // Mesh/CascadedShadow.glslh, included right below because it names what is declared here.
        // DESERT_SUN_SHADOW_SCREEN_MASK (DeferredLightingRenderer's second program, "Shadows: Ray Traced Sun"): the
        // sun's visibility is the denoised screen mask, and the cascades are compiled out.
        #ifdef DESERT_SUN_SHADOW_SCREEN_MASK
        Uniform(5) texture2D ScreenShadowMaskTexture; // r = visibility, 1 = lit (render extent = this target's)
        #define ScreenShadowMaskTexture sampler2D(ScreenShadowMaskTexture, PointClampSampler)
        // The other directional lights keep their cascades under the sun's mask (DeferredScreenShadowMask::Lights).
        Uniform(10) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)
        #else
        // The four cascades are ONE array texture (layer c = cascade c), read through the shared repeat state.
        Uniform(5) texture2DArray ShadowCascadesTexture;
        #define ShadowCascadesTexture sampler2DArray(ShadowCascadesTexture, LinearRepeatSampler)
        #endif
        #include <Mesh/ShadowUB.glslh> // the view's ShadowUB, binding 7 (SceneResources::ShadowUBData)

        // The light a subsurface receiver lets through takes its own occlusion (DesertLight::TransmissionShadow):
        // with cascades, their thickness transmittance; with a screen mask (ray-traced or virtual shadow maps) the
        // mask itself, as UE's ray-traced shadows feed the same mask to the subsurface transmission.
        #ifdef DESERT_SUN_SHADOW_SCREEN_MASK
        #include <Mesh/CascadedShadow.glslh>
        #define DESERT_SUN_SHADOW(worldPos, N, L, cascade) texelFetch(ScreenShadowMaskTexture, ivec2(gl_FragCoord.xy), 0).r
        #define DESERT_SUN_TRANSMISSION_SHADOW(worldPos, density) texelFetch(ScreenShadowMaskTexture, ivec2(gl_FragCoord.xy), 0).r
        #else
        #include <Mesh/CascadedShadow.glslh>
        #define DESERT_SUN_SHADOW(worldPos, N, L, cascade) ShadowFactor(kShadowLightSun, worldPos, N, L, cascade)
        #define DESERT_SUN_TRANSMISSION_SHADOW(worldPos, density) TransmissionShadowFactor(kShadowLightSun, worldPos, density)
        #endif

        // THE CONTACT SHADOWS (UE ContactShadowLength): a short screen-space ray toward each light through
        // GBufferDepthTexture. Included after ShadowUB, which carries the march's view values; Mesh/LocalLightShadow.glslh
        // below applies it to the point and spot lights, the sun's is applied in main().
        #define CONTACT_SHADOW_DEPTH GBufferDepthTexture
        #define CONTACT_SHADOW_VP    JitteredWorldToClip
        #define CONTACT_SHADOW_INV_VP InverseJitteredViewProjection
        // The hit texel's NO_CONTACT_SHADOW bit (UE CastContactShadow( GBuffer )): which intensity occludes.
        #define CONTACT_SHADOW_SHADING_WORD GBufferShadingWordTexture
        #include <Common/ContactShadow.glslh>

        // THE CLOUD LAYER'S SHADOW ON THE WORLD — a second, independent occluder of the same sun.
        // Declared here, evaluated by the shared receiver text below: the wrapper used to live in this
        // file and this file ALONE, which is exactly why no forward-shaded surface in the engine ever
        // received a cloud shadow. See Common/CloudShadowReceiver.glslh.
        layout(set = 0, binding = 11) uniform texture2D CloudShadowMapTexture;
        #define CloudShadowMapTexture sampler2D(CloudShadowMapTexture, LinearRepeatSampler)
        Uniform(12) CloudShadowUB {
        	mat4 CloudShadowWorldToMap;
        	// x = the kilometres the map's clip z spans, y = 1 when the map is real and must be read,
        	// z = the UV width of the border fade, w = the artist's shadow strength.
        	vec4 CloudShadowParams;
        };

        // THE receiver, shared verbatim with the forward mesh shaders, the glass pass and the terrain.
        // Included after the two bindings above because it names them.
        #include <Common/CloudShadowReceiver.glslh>

        // THE POINT AND SPOT LIGHTS' SHADOWS: the view's local shadow atlas and the table of its views
        // (Graphic/LocalShadows.hpp; MeshRenderer::UpdateLocalShadows writes both, and each light's shadowView).
        // The row struct first, then the two bindings, then the reader, which names them.
        #ifndef DESERT_MEGALIGHTS
        #ifdef DESERT_LOCAL_SHADOW_VSM
        // Shadows.Method VirtualMaps: the shadow is "VSM: Project Local"'s mask (below), the subsurface transmission
        // the virtual map itself through the reader the mask was made with (Common/VirtualShadowMap.glslh); no atlas.
        #include <Common/VirtualShadowMap.glslh>
        ReadBuffer(13) VsmFrameData
        {
        	VsmFrame Frame;
        	VsmLevel Levels[];
        };
        ReadBuffer(14) VsmPageTable
        {
        	VsmPageEntry Entries[];
        };
        layout(set = 0, binding = 33, r32ui) restrict readonly uniform uimage2D PhysicalPoolTexture; // 33: MegaLights' only
        ReadBuffer(15) VsmLocalLightsUB
        {
        	VsmLocalLight vsmLocalLights[];
        };
        #define DESERT_VSM_READ
        #include <Common/VirtualShadowMap.glslh>
        #else
        #include <Mesh/LocalLightShadowView.glslh>
        Uniform(13) texture2D LocalShadowAtlasTexture; // 13/14: freed by the cascade array
        #define LocalShadowAtlasTexture sampler2D(LocalShadowAtlasTexture, PointClampSampler)
        ReadBuffer(14) LocalShadowViewsUB
        {
        	LocalShadowView localShadowViews[];
        };
        #endif
        #ifdef DESERT_LOCAL_SHADOW_SCREEN_MASK
        // "Shadows: Ray Traced Local" (Shadows HardwareTraced): one layer per shadowed light, by its shadowMaskSlot
        // (Mesh/LocalLightShadow.glslh reads it before the light's shadow view). 32 is MegaLights' only.
        Uniform(32) texture2DArray LocalShadowMasksTexture;
        #define DesertLocalShadowScreenMask(slot) texelFetch(sampler2DArray(LocalShadowMasksTexture, PointClampSampler), ivec3(ivec2(gl_FragCoord.xy), slot), 0).r
        #endif
        #include <Mesh/LocalLightShadow.glslh>
        #else
        // MegaLights' local lighting (System::MegaLightsRenderer: ReSTIR DI over the light grid, ray-query visibility,
        // the screen-space denoiser), as two demodulated signals: diffuse * MegaLightsDiffuseReflectance plus
        // specular * MegaLightsSpecularReflectance of this texel is its light.
        Uniform(32) texture2D MegaLightsDiffuseTexture;
        #define MegaLightsDiffuseTexture sampler2D(MegaLightsDiffuseTexture, PointClampSampler)
        Uniform(33) texture2D MegaLightsSpecularTexture;
        #define MegaLightsSpecularTexture sampler2D(MegaLightsSpecularTexture, PointClampSampler)
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
        // The specular half of ShadeGridLight (the screen-space scattered models' split, CONTRACT §4).
        vec3 ShadeGridLightSpecular(uint light, int shadingModel, vec3 worldPos, vec3 N, float subsurfaceOpacity,
                                    DesertSurface surface, DesertPayload payload)
        {
        	if (light < lightsMetadata.PointLightCount)
        		return DesertEvaluateShadingModelSpecular(shadingModel,
        		                                          DesertShadowedPointLightAt(pointLights[light], worldPos, N, subsurfaceOpacity),
        		                                          surface, payload);
        	return DesertEvaluateShadingModelSpecular(shadingModel,
        	                                          DesertShadowedSpotLightAt(spotLights[light - lightsMetadata.PointLightCount],
        	                                                                    worldPos, N, subsurfaceOpacity),
        	                                          surface, payload);
        }

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

        #ifdef DESERT_DEFERRED_DIRECT
        void main()
        {
        	vec4 ga = texture(GBufferATexture, v_TexCoord);
        	vec4 gb = texture(GBufferBTexture, v_TexCoord);

        	vec3  albedo    = ga.rgb;
        	float metallic  = ga.a;
        	vec3  normal    = gb.rgb;
        	float roughness = max(gb.a, kDesertMinRoughness);
        	vec3  worldPos  = ReconstructWorldPosition(v_TexCoord, texture(GBufferDepthTexture, v_TexCoord).r,
        	                                           InverseJitteredViewProjection);

        	const int dbg = int(Params.x + 0.5);

        	// No geometry: the composite answers the texel (its discard, or a G-buffer view's black) unread.
        	const bool hasGeometry = dot(normal, normal) > 0.001;
        	if (!hasGeometry)
        		discard;

        	const uvec2         word         = texelFetch(GBufferShadingWordTexture, ivec2(gl_FragCoord.xy), 0).rg;
        	const int           shadingModel = DesertShadingModelIndex(word);
        	const DesertPayload payload      = DesertUnpackPayload(word);

        	// The G-buffer views the composite draws from the G-buffer alone (1..5, 9..11): nothing to light.
        	if ((dbg >= 1 && dbg <= 5) || (dbg >= 9 && dbg <= 11))
        	{
        		oDirect = vec4(0.0, 0.0, 0.0, kDeferredDirectUntinted);
        		return;
        	}

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
        		oDirect = vec4(HeatColor(float(lights) / 8.0) * CompositeView.y, kDeferredDirectShown);
        		return;
        	}

        	// --- Lit: shadow-mapped directional sun (N·L) + full lit point/spot lights ---
        	vec3 N    = normalize(normal);
        	vec3 view = normalize(CameraPosition.xyz - worldPos);

        	// Directional sun (energy-normalized lit), occluded by the cascaded shadow map.
        	// TWO OCCLUDERS OF ONE SUN, multiplied: the cascaded maps for opaque geometry and the cloud
        	// layer's own volumetric transmittance. Only the DIRECTIONAL term is attenuated — the ambient
        	// below is the whole sky dome, which a cloud deck occludes with a different geometry
        	// (a hemisphere, not a direction) and which UE answers with a separate Sky AO volume. Folding
        	// a directional occlusion into an omnidirectional term would darken the shaded side of every
        	// object under a cloud by an amount nothing in the world justifies.
        	vec3  L        = normalize(-LightDirection.xyz);
        	// `cascade` is the sun's chosen cascade index (-1 = past the cascades, or the screen-mask program), read by
        	// the Shadow Cascades view below (ShadowDebugMode, ShadowUB DebugParams.z — the slot the forward mesh
        	// shaders read it from), not by a second ShadowFactor overload: a second overload is how the four copies
        	// started.
        	int   cascade  = -1;
        	// The cascades skip a surface whose material does not receive them (the shading word's NO_SUN_SHADOWS bit), then the
        	// cloud layer — the cloud factor AFTER the toggle, exactly as the forward pass assembles it.
        	// The sun's contact shadow joins its cascades: it is the near-contact part of the same occlusion.
        	// The receiver's subsurface opacity (UE IsSubsurfaceModel + CustomData.a; -1 = not subsurface): its
        	// contact shadows fade with the hit distance.
        	const float subsurfaceOpacity = DesertSubsurfaceOpacity(shadingModel, payload);
        	const bool  receivesSun       = DesertReceivesSunShadows(word);
        	const float sunContact =
        	     receivesSun ? ContactShadowFactor(worldPos, N, L, SunContactShadow, subsurfaceOpacity) : 1.0;
        	float shadow   = receivesSun ? DESERT_SUN_SHADOW(worldPos, N, L, cascade) * sunContact : 1.0;
        	// The relief's own shadow (parallax occlusion, the word's SUN_SELF_SHADOW field): geometry, always applied.
        	shadow        *= DesertSunSelfShadow(word);
        	// SHADOW VIEWS (UE's CSM visualization in the lighting pass; ShadowDebugMode, DebugParams.z):
        	// 1 = Shadow Factor - the sun's geometry occlusion alone (cascades or screen mask, contact, relief; the
        	// cloud layer is a separate occluder and stays out), grey with no albedo, pre-exposed like every channel.
        	// 2 = Shadow Cascades - the lit colour tinted by `cascade` at the end of main().
        	const int shadowView = int(DebugParams.z + 0.5);
        	if (shadowView == 1) { oDirect = vec4(vec3(shadow) * CompositeView.y, kDeferredDirectShown); return; }
        	// The light a subsurface receiver lets through takes its own occlusion (DesertLight::TransmissionShadow):
        	// the cascades' thickness transmittance, not ShadowFactor, whose normal offset puts a leaf's far side
        	// behind the leaf. 1 for every other model (none reads it).
        	float transmissionShadow =
        	     (receivesSun && subsurfaceOpacity >= 0.0)
        	          ? DESERT_SUN_TRANSMISSION_SHADOW(worldPos, ContactShadowSubsurfaceDensity(subsurfaceOpacity)) * sunContact
        	          : 1.0;
        	const float cloudShadow = CloudShadowFactor(worldPos);
        	shadow             *= cloudShadow;
        	transmissionShadow *= cloudShadow;
        	vec3  radiance = LightColor.rgb * LightColor.a;

        	// The screen-space scattered texel (CONTRACT §4) is lit with BaseColor = 1: its diffuse leaves demodulated,
        	// so the scatter does not smear the albedo's texture detail; Recombine multiplies BaseColor back.
        	const bool scatters = CompositeView.z > 0.5 && DesertShadingModelScattersInScreenSpace(shadingModel) &&
        	                      DesertSubsurfaceProfileIndex(shadingModel, payload) != 255u;
        	if (scatters)
        		albedo = vec3(1.0);
        	const DesertSurface surface =
        	     DesertMakeSurface(worldPos, N, view, albedo, metallic, roughness, DesertShadingWordSpecular(word));

        	const DesertLight sunLight = DesertSunLight(LightDirection.xyz, radiance, shadow, transmissionShadow);
        	vec3 result   = DesertEvaluateShadingModel(shadingModel, sunLight, surface, payload);
        	// The specular half of `result`, summed only for a scattered texel (diffuse = result - specular).
        	vec3 specular = scatters ? DesertEvaluateShadingModelSpecular(shadingModel, sunLight, surface, payload)
        	                         : vec3(0.0);

        	// ATMOSPHERE LIGHT 1 (UE's Atmosphere Sun Light Index 1, the moon): the same shading model, its own
        	// direction and transmitted illuminance, occluded by ITS OWN cascades (slot kShadowLightAtmosphere1 of the
        	// one CascadedShadow text; UE: a CSM per directional light). The contact march, the relief self-shadow and
        	// the cloud shadow map are fitted to the SUN's direction, so none of them occludes this light. Absent
        	// (a == 0) adds nothing at all.
        	if (Light1Color.a > 0.0)
        	{
        		const vec3 L1 = normalize(-Light1Direction.xyz);
        		int        cascade1 = -1;
        		const float shadow1 =
        		     receivesSun ? ShadowFactor(kShadowLightAtmosphere1, worldPos, N, L1, cascade1) : 1.0;
        		const float transmission1 =
        		     (receivesSun && subsurfaceOpacity >= 0.0)
        		          ? TransmissionShadowFactor(kShadowLightAtmosphere1, worldPos,
        		                                     ContactShadowSubsurfaceDensity(subsurfaceOpacity))
        		          : 1.0;
        		const DesertLight light1 =
        		     DesertSunLight(Light1Direction.xyz, Light1Color.rgb * Light1Color.a, shadow1, transmission1);
        		result += DesertEvaluateShadingModel(shadingModel, light1, surface, payload);
        		if (scatters)
        			specular += DesertEvaluateShadingModelSpecular(shadingModel, light1, surface, payload);
        	}

        	// Point and spot lights (the city payoff): full Cook-Torrance lit, each occluded by its atlas views when it
        	// casts shadows — the lights of this pixel's froxel only, in the order the full loop would sum them. An
        	// overflowed cell (more lights than its list holds) is shaded by every light: slower, never wrong.
        	// Under MegaLights the same lights arrive sampled, traced and denoised: the texel remodulated by the
        	// reflectances they were demodulated with ("MegaLights: Shade Samples"), diffuse and specular apart.
        #ifdef DESERT_MEGALIGHTS
        	const vec3 megaLightsSpecular = texelFetch(MegaLightsSpecularTexture, ivec2(gl_FragCoord.xy), 0).rgb *
        	                                MegaLightsSpecularReflectance(albedo, metallic, roughness, N, view);
        	result += texelFetch(MegaLightsDiffuseTexture, ivec2(gl_FragCoord.xy), 0).rgb *
        	              MegaLightsDiffuseReflectance(albedo, metallic) +
        	          megaLightsSpecular;
        	specular += megaLightsSpecular;
        #else
        	const uint cell  = LightGridCellAt(lightGrid, gl_FragCoord.xy, worldPos);
        	const uint count = lightGridCells[cell];
        	if (!LightGridCellOverflowed(lightGrid, count))
        	{
        		const uint list = LightGridListOffset(lightGrid, cell);
        		for (uint j = 0u; j < count; j++)
        		{
        			result += ShadeGridLight(lightGridCells[list + j], shadingModel, worldPos, N, subsurfaceOpacity, surface,
        			                         payload);
        			if (scatters)
        				specular += ShadeGridLightSpecular(lightGridCells[list + j], shadingModel, worldPos, N,
        				                                   subsurfaceOpacity, surface, payload);
        		}
        	}
        	else
        	{
        		const uint lights = lightsMetadata.PointLightCount + lightsMetadata.SpotLightCount;
        		for (uint i = 0u; i < lights; i++)
        		{
        			result += ShadeGridLight(i, shadingModel, worldPos, N, subsurfaceOpacity, surface, payload);
        			if (scatters)
        				specular += ShadeGridLightSpecular(i, shadingModel, worldPos, N, subsurfaceOpacity, surface, payload);
        		}
        	}
        #endif

        	// The composite's inputs: the direct sum, the cascade the Shadow Cascades view tints, and a scattered
        	// texel's specular half, which its subsurface split takes off (diffuse = result - specular).
        	if (scatters)
        		imageStore(DeferredDirectSpecularImage, ivec2(gl_FragCoord.xy), vec4(specular, 0.0));
        	const bool tinted = shadowView == 2 && cascade >= 0;
        	oDirect = vec4(result, tinted ? float(min(cascade, 3)) : kDeferredDirectUntinted);
        }
        #else
        void main()
        {
        	vec4 ga = texture(GBufferATexture, v_TexCoord);
        	vec4 gb = texture(GBufferBTexture, v_TexCoord);

        	vec3  albedo    = ga.rgb;
        	float metallic  = ga.a;
        	vec3  normal    = gb.rgb;
        	float roughness = max(gb.a, kDesertMinRoughness);
        	vec3  worldPos  = ReconstructWorldPosition(v_TexCoord, texture(GBufferDepthTexture, v_TexCoord).r,
        	                                           InverseJitteredViewProjection);

        	const int dbg = int(Params.x + 0.5);

        	// No geometry here (zero normal — the G-buffer is cleared to 0): discard so the LOADed forward scene
        	// (real procedural sky / skybox + grid) shows through. A G-BUFFER VIEW (UE Buffer Visualization:
        	// BaseColor, WorldNormal, Metallic, Roughness, AO, ShadingModel; the same set SceneRenderer composites no
        	// fog or clouds over) shows the stored surface, and the sky stores none: black there, as UE's BaseColor.
        	const bool hasGeometry = dot(normal, normal) > 0.001;
        	if (!hasGeometry)
        	{
        		if ((dbg >= 1 && dbg <= 5) || dbg == 10 || dbg == 11)
        		{
        			oColor = vec4(0.0, 0.0, 0.0, 1.0);
        			return;
        		}
        		discard;
        	}

        	const vec4          ge           = texture(GBufferEmissiveTexture, v_TexCoord);
        	const uvec2         word         = texelFetch(GBufferShadingWordTexture, ivec2(gl_FragCoord.xy), 0).rg;
        	const int           shadingModel = DesertShadingModelIndex(word);
        	const DesertPayload payload      = DesertUnpackPayload(word);

        	// Ambient occlusion as UE composes it for the ambient: the MATERIAL's (GBuffer AO, from the surface's
        	// AmbientOcclusion output — the forward path's whole AO) times the screen-space one. z = SSAO enabled.
        	const float ssao = (Params.z > 0.5) ? texture(SsaoTexture, v_TexCoord).r : 1.0;
        	float       ao   = ge.a * ssao;

        	// --- Debug channels (shown on the geometry, over the sky) ---
        	// Each is stored pre-exposed like the lit result (x CompositeView.y = PreExposure): the tonemap's
        	// 1/PreExposure then leaves the channel as authored.
        	if (dbg == 1) { oColor = vec4(albedo, 1.0); oColor.rgb *= CompositeView.y; return; }                          // Albedo
        	if (dbg == 2) { oColor = vec4(normalize(normal) * 0.5 + 0.5, 1.0); oColor.rgb *= CompositeView.y; return; }   // Normal
        	if (dbg == 3) { oColor = vec4(vec3(metallic),  1.0); oColor.rgb *= CompositeView.y; return; }                 // Metallic
        	if (dbg == 4) { oColor = vec4(vec3(roughness), 1.0); oColor.rgb *= CompositeView.y; return; }                 // Roughness
        	if (dbg == 5) { oColor = vec4(vec3(ao),        1.0); oColor.rgb *= CompositeView.y; return; }                 // Ambient Occlusion

        	// THE DIRECT LIGHT of this texel, summed by "Deferred: Direct Lighting" (the program above).
        	const vec4 direct = texelFetch(DeferredDirectLightingTexture, ivec2(gl_FragCoord.xy), 0);
        	// Light Complexity (7) and Shadow Factor (DebugParams.z 1) are the direct pass's views, pre-exposed.
        	if (direct.a == kDeferredDirectShown) { oColor = vec4(direct.rgb, 1.0); return; }

        	// Material Complexity: the G-buffer pass stashed the material's sampled-texture count in the shading word's TEXTURES field
        	// (0..3). Heat-map it as a proxy for per-pixel shading cost (UE-style shader/material complexity).
        	if (dbg == 9) { oColor = vec4(HeatColor(DesertSampledTextureCount(word) / 3.0), 1.0); oColor.rgb *= CompositeView.y; return; }

        	// Shading Model / Sun Shadow Receive: the word's other two fields, decoded from the same integer fetch (the
        	// slot is RG32_UINT; a float view of it would reinterpret the bits). One flat colour per model index (golden-
        	// ratio hue walk, so neighbouring indices differ); green = receives the sun's cascades, red = opted out.
        	if (dbg == 10)
        	{
        		const float hue = fract(float(shadingModel) * 0.618034);
        		oColor          = vec4(clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0), 1.0); oColor.rgb *= CompositeView.y;
        		return;
        	}
        	if (dbg == 11) { oColor = DesertReceivesSunShadows(word) ? vec4(0.1, 0.9, 0.2, 1.0) : vec4(0.9, 0.1, 0.1, 1.0); oColor.rgb *= CompositeView.y; return; }

        	// --- Lit: the direct pass's sum + the ambient, the emissive and the subsurface split ---
        	vec3 N    = normalize(normal);
        	vec3 view = normalize(CameraPosition.xyz - worldPos);

        	// The screen-space scattered texel (CONTRACT §4) is lit with BaseColor = 1: its diffuse leaves demodulated,
        	// so the scatter does not smear the albedo's texture detail; Recombine multiplies BaseColor back.
        	const bool scatters = CompositeView.z > 0.5 && DesertShadingModelScattersInScreenSpace(shadingModel) &&
        	                      DesertSubsurfaceProfileIndex(shadingModel, payload) != 255u;
        	if (scatters)
        		albedo = vec3(1.0);
        	const DesertSurface surface =
        	     DesertMakeSurface(worldPos, N, view, albedo, metallic, roughness, DesertShadingWordSpecular(word));
        	// The direct pass's sum, and a scattered texel's specular half of it.
        	vec3 result   = direct.rgb;
        	vec3 specular = scatters ? texelFetch(DeferredDirectSpecularTexture, ivec2(gl_FragCoord.xy), 0).rgb
        	                         : vec3(0.0);

        	// The GI method's final gather (Params.w): 0 none (the sky's two cubes), 2 the traced gather, 3 the
        	// same gather with its own short-range AO. GI comes from the method alone: the composite adds no
        	// bounce of its own (its sun-only screen-space neighbour gather is gone, GTX-TIER-LADDER).
        	const int giMode = int(Params.w + 0.5);
        	// Indirect GI only (UE: the Lumen Global Illumination view): the traced gather is the ambient's diffuse
        	// below, where it replaced the sky terms; a view without one has no indirect GI to show.
        	if (dbg == 6 && giMode < 2) { oColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

        	// Ambient = the BAKED ENVIRONMENT (split-sum IBL: diffuse irradiance + prefiltered specular),
        	// attenuated by SSAO — assembled by the SAME shared function the forward mesh shaders call, so a
        	// scene rendered through either RenderingPath gets one ambient model. Its `indirect` is zero here:
        	// the forward cells pass their MegaLights volume's local light there (AmbientIBL.glslh
        	// DesertSampleViewAmbient); this path's local lights are the direct pass's.
        	// The specular radiance along the reflection is the traced one where SSR answered, the indirect source's
        	// for the rest: the traced GI's final gather (screen probes or the irradiance field), else the reflection
        	// environment (captures, then sky) (UE ReflectionEnvironmentPixelShader: SSR over the rest, then the
        	// split-sum Fresnel of both together).
        	// GI modes 2 and 3: the traced final gather at this pixel (one texel each, the G-buffer's extent) is the estimator -
        	// its diffuse irradiance and specular radiance replace both sky terms, so a closed room neither glows nor
        	// mirrors the sky. The captures compose over its specular and are normalised by its diffuse (the radiance
        	// cache measured their capture points: ReflectionCaptureRenderer AddCaptureUpdate).
        	DesertAmbient sampled = giMode >= 2
        	                             ? DesertSampleAmbient(surface, texelFetch(GiDiffuseTexture, ivec2(gl_FragCoord.xy), 0).rgb,
        	                                                   texelFetch(GiSpecularTexture, ivec2(gl_FragCoord.xy), 0).rgb,
        	                                                   vec3(0.0), ao)
        	                             : DesertSampleAmbient(surface, vec3(0.0), ao);
        	// The reflection environment along the lobe: what a traced ray's miss sees (the captures, over the traced
        	// gather's specular in GI modes 2 and 3, else over the sky).
        	const vec3 environment     = sampled.Prefiltered;
        	const int  reflectionInput = int(CameraPosition.w + 0.5);
        	// DeferredDebugMode::Reflections (18): the traced reflection alone - SsrTexture's radiance by its coverage over
        	// black, so a miss, the environment, the GI's rough specular and the direct light are all absent. Any other
        	// input is not a traced reflection: black, never the screen trace (SceneRenderer::AddFrameComposite refuses
        	// it by name).
        	if (dbg == 18)
        	{
        		oColor = reflectionInput == REFLECTION_INPUT_TRACED
        		             ? vec4(DesertComposeTracedReflection(textureLod(SsrTexture, v_TexCoord, 0.0), vec3(0.0)), 1.0)
        		             : vec4(0.0, 0.0, 0.0, 1.0);
        		return;
        	}
        	if (giMode >= 2)
        	{
        		// The traced gather replaced the sky terms; a two-sided leaf's far side is lit by its -N gather.
        		sampled.Traced         = 1.0;
        		sampled.BackIrradiance = texelFetch(GiBackfaceTexture, ivec2(gl_FragCoord.xy), 0).rgb;
        		// giMode 3: the gather carries its own short-range AO (bent normal, ScreenProbeShortRangeAO), so only
        		// the material's occlusion is left to apply (UE r.Lumen.DiffuseIndirect.SSAO 0 — no SSAO on Lumen's
        		// indirect). giMode 2 (the irradiance field, or the short-range AO off): SSAO occludes it, as before.
        		if (giMode == 3)
        			sampled.Occlusion = ge.a;
        	}
        	if (reflectionInput == REFLECTION_INPUT_TRACED)
        	{
        		// UE MixSpecularAndRoughReflections (DiffuseIndirectComposite.usf) for Lumen reflections: the traced
        		// reflection - its hits, its misses answered by the captures, then the sky (better than UE's sky
        		// alone indoors) - answers TracedReflectionsAlpha of the lobe, the traced GI's rough specular the
        		// rest. Without traced GI there is no rough specular: the traced reflection answers the whole lobe,
        		// its screen hits past the max roughness included, as ScreenSpace's do.
        		const vec3 traced = DesertComposeTracedReflection(textureLod(SsrTexture, v_TexCoord, 0.0), environment);
        		sampled.Prefiltered = giMode >= 2 ? mix(sampled.Prefiltered, traced, TracedReflectionsAlpha(gb.a)) : traced;
        	}
        	else if (reflectionInput == REFLECTION_INPUT_SCREEN)
        		// UE's SSR input: the screen's hits over the GI's specular (or the environment), by their coverage.
        		sampled.Prefiltered = DesertComposeTracedReflection(textureLod(SsrTexture, v_TexCoord, 0.0), sampled.Prefiltered);
        	vec3 ambient = DesertEvaluateShadingModelAmbient(shadingModel, sampled, surface, payload);
        	if (dbg == 6)
        	{
        		const vec3 ambientDiffuse =
        		     ambient - DesertEvaluateShadingModelAmbientSpecular(shadingModel, sampled, surface, payload);
        		oColor = vec4(max(ambientDiffuse, vec3(0.0)) * CompositeView.y, 1.0);
        		return;
        	}

        	// Self-illumination (view-independent) — added here (not lit) so HDR emissive reaches the composite
        	// and blooms, matching the forward path. GBufferEmissive is 0 where the material has none.
        	vec3 emissive = ge.rgb;

        	// The view scatters (CONTRACT §4): a scattered texel splits, every other geometry texel marks itself
        	// unscattered (the target is a transient: every texel the scatter reads is written here or is sky).
        	if (CompositeView.z > 0.5)
        	{
        		if (scatters)
        		{
        			const vec3 ambientSpecular =
        			     DesertEvaluateShadingModelAmbientSpecular(shadingModel, sampled, surface, payload);
        			const vec3 diffuse = max(result + ambient - specular - ambientSpecular, vec3(0.0));
        			imageStore(SubsurfaceDiffuseTexture, ivec2(gl_FragCoord.xy),
        			           vec4(diffuse * CompositeView.y, float(DesertSubsurfaceProfileIndex(shadingModel, payload)) + 1.0));
        			oColor = vec4((specular + ambientSpecular + emissive) * CompositeView.y, 1.0);
        			return;
        		}
        		imageStore(SubsurfaceDiffuseTexture, ivec2(gl_FragCoord.xy), vec4(0.0));
        	}

        	// Stored pre-exposed (ViewFrame::PreExposure): a 100 000 lx sun stays inside RGBA16F.
        	oColor = vec4((result + ambient + emissive) * CompositeView.y, 1.0);
        	// Shadow Cascades: red / green / blue / yellow front to back (the forward mesh shaders' order); a pixel
        	// no cascade covers keeps its lit colour.
        	if (direct.a >= 0.0)
        	{
        		const vec3 kCascadeTint[4] = vec3[4](vec3(1.0, 0.25, 0.25), vec3(0.25, 1.0, 0.25),
        		                                     vec3(0.25, 0.25, 1.0), vec3(1.0, 1.0, 0.25));
        		oColor.rgb *= kCascadeTint[int(direct.a)];
        	}
        }
        #endif
    }
}

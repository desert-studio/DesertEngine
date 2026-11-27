#pragma once

// THE CLOUD BLOCKS, declared once: Tools/ShaderProtocolGen writes their GLSL into Generated/Clouds/*.glslh
// (included by Common/CloudParams.glslh, Common/CloudResolveParams.glslh, Common/CloudAuthored.glslh, the cloud
// shadow receivers and Programs/Clouds/CloudRaymarch.shader) and checks every layout against these declarations
// at compile time. The packers stay with their subsystems (CloudPayload.hpp, CloudAuthoredPayload.hpp,
// CloudShadowPayload.hpp).
//
// Units: KILOMETRES in the march's blocks (the packers convert world units once); the shadow maps' matrices take
// world centimetres.

#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic
{
    /// How many clip levels the block carries (CloudGpuPayload::Level, CloudLevel). Pinned equal to
    /// Assets::kCloudProceduralClipLevels in CloudProceduralClipmap.cpp; a number here because this header
    /// is the GPU layout and does not pull the bake in.
    inline constexpr uint32_t kCloudClipLevelSlots = 3u;
    /// The far path's height bands (Assets::kCloudFarBands; the renderer asserts the two agree).
    inline constexpr uint32_t kCloudFarBandSlots = 8u;

    // The cloud march's setter-bound parameter block (the authored instance buffer is the other one): handed to
    // SetStorageBuffer verbatim, and the generated block's CLOUD_PARAMS_BINDING default.
    inline constexpr uint32_t kCloudParamsBinding = 1;

    // The TEMPORAL RESOLVE pass's block (Programs/Clouds/CloudTemporalResolve.shader), handed to SetStorageBuffer
    // verbatim and written into the generated block.
    inline constexpr uint32_t kCloudResolveParamsBinding = 6;

    /**
     * The GPU side of VolumetricCloudData (packed by PackCloudParams, CloudPayload.hpp): CloudParamsBuffer.
     *
     * Units: KILOMETRES throughout. The packer is where the component's world-unit distances are converted,
     * exactly once.
     *
     * EVERY SLOT IS READ, OR IT HAS A ROW IN kCloudUnreadSlots SAYING WHY NOT. There is no reserved field: a
     * spare slot is where a future parameter gets quietly stashed without a name, a range or a tooltip. The
     * unread slots are the toll std430 charges for a grid of vec4s — see kCloudUnreadSlots, the register
     * Desert/Tests/Engine/CloudMediumConsumers checks the shader text against.
     *
     * TWENTY-EIGHT FLOATS ARE THE FOUR SPECIES' — SpeciesEdge's sixteen, SpeciesNoise's four and the wispy
     * base's eight (SpeciesWispBase, SpeciesWispTop): with four kinds of cloud in one shell no per-layer product
     * of a species' factors can be formed once, so each is a per-species number.
     */
    using Vec4ArrayCloudGpuPayloadLevel = std::array<glm::vec4, kCloudClipLevelSlots>;
    using Vec4ArrayCloudGpuPayloadSpeciesEdge = std::array<glm::vec4, kCloudSpeciesSlots>;
    using Vec4ArrayCloudGpuPayloadFarPresence = std::array<glm::vec4, kCloudFarBandSlots>;
    using Vec4ArrayCloudGpuPayloadFarProfile = std::array<glm::vec4, kCloudFarBandSlots>;
#define DESERT_CLOUD_GPU_PAYLOAD_FIELDS( X )                                                                           \
    X( glm::vec4, Layer, "CloudLayerGeometry", {} ) /* x planet radius, y bottom altitude, z thickness, w max view distance (km) */ \
    /* w is the LAYER's extinction alone; the winning species' factor multiplies it in the march. */                   \
    X( glm::vec4, March, "CloudMarch", {} ) /* x max steps, y stop transmittance, z tracing start (km), w extinction (1/km) */ \
    /* THE MODELLING VOLUME'S REGION, IN THE SLOT THE WEATHER SETTINGS VACATED. */                                     \
    /*  */                                                                                                             \
    /* This vec4 held (coverage tile, coverage, coverage contrast, detail tile). Three of those four */                \
    /* decided WHERE cloud is, and none of them decides it any more: the lattice a cluster of lumps is */              \
    /* drawn in, the fraction of cells that carry one, and how sharply a cell fills are all consumed by */             \
    /* Assets::BakeCloudProceduralVolume on the CPU, at bake time. Sending them to the march as well */                \
    /* would be three dead slots in a block whose whole discipline is that every slot has a consumer — */              \
    /* so the region moved in and the fourth number, which the erosion still reads, stayed put. */                     \
    /*  */                                                                                                             \
    /* xy the region's minimum corner in world kilometres, SNAPPED to the lump lattice by */                           \
    /* Assets::CloudProceduralRegionOriginKm; z the RECIPROCAL of its horizontal side, because the march */            \
    /* only ever divides by it; w the erosion's period, unchanged. */                                                  \
    /* CLIP-2: THE CLIPMAP'S THREE LEVELS, finest first (Graphic::CloudProceduralClipmap::LevelUniforms): */           \
    /* xy the level's minimum corner on the device, world km; z 1 / its side; w its voxel, km. The detail */           \
    /* tile that shared the old region vec4 moved to Detail.z, the slot the scalar albedo vacated. */                  \
    X( Vec4ArrayCloudGpuPayloadLevel, Level, "CloudLevel", {} )                                                        \
    /* z HELD THE SCALAR SCATTERING ALBEDO and holds nothing now — the albedo became a colour and moved */             \
    /* to its own vec4 below. It is not reused and not renamed to a pad: kUnreadSlots carries its row. */              \
    X( glm::vec4, Detail, "CloudDetail", {} ) /* x detail strength, y density scale, z detail tile (km), w species count */ \
    X( glm::vec4, Wind, "CloudWind", {} ) /* xyz accumulated wind offset (km), w phase g */                            \
    X( glm::vec4, Sun, "CloudSun", {} ) /* xyz TOWARD the sun (normalized), w light march distance (km) */             \
    X( glm::vec4, SunColour, "CloudSunColour", {} ) /* rgb sun irradiance (linear), w light march sample count */      \
    X( glm::vec4, Ambient, "CloudAmbient", {} ) /* rgb ambient radiance OR its scale, w = 1 when the distant sky light is read */ \
    X( glm::vec4, MultiScatter, "CloudMultiScatter", {} ) /* x octave count, y contribution, z occlusion, w eccentricity */ \
    X( glm::vec4, Phase, "CloudPhase", {} ) /* x second lobe g, y phase blend, z AO strength, w tracing start max (km) */ \
    X( glm::vec4, Fade, "CloudFadeParams", {} ) /* x AP start (km), y AP fade (km), z near fade end (km), w near fade start (km) */ \
    /* THE FOUR SPECIES. Both arrays are indexed by the same slot, and slots at or past Detail.w are */                \
    /* zero — the loop in the march stops at the count, and an unfilled slot's channel of the profile */               \
    /* table is zero at every altitude anyway, so there are two independent reasons it is never read. */               \
    /*  */                                                                                                             \
    /* x DetailCharacter, y DetailFactor, z DensityFactor, w ExtinctionFactor: what that kind of cloud */              \
    /* is MADE OF. The winner of the union at a sample takes all four; nothing is averaged. */                         \
    X( Vec4ArrayCloudGpuPayloadSpeciesEdge, SpeciesEdge, "CloudSpeciesEdge", {} )                                      \
    /* WHICH NOISE VOLUME EACH SPECIES' EDGE IS CUT FROM — x for species 0, y for 1, z for 2, w for 3, */              \
    /* each of them an index into the four `sampler3D` slots the march binds. Four floats and four */                  \
    /* consumers, which is the same discipline the array above is held to. */                                          \
    /*  */                                                                                                             \
    /* IT IS A DEDUPLICATED INDEX AND NOT THE SPECIES NUMBER, and the difference is the whole reason */                \
    /* this vec4 exists rather than the march simply reading `sampler[speciesIndex]`. Eight of the nine */             \
    /* shipped types name no volume of their own, so the ordinary four-species sky reads ONE volume */                 \
    /* through four descriptors — and a fetch selected by a value that varies across a wave costs the */               \
    /* wave every branch it takes, whether or not the branches land on the same image. Deduplicated, */                \
    /* that sky sends {0,0,0,0}, the compare chain is uniformly false, and the march does exactly what */              \
    /* it did before this field existed. Graphic::ResolveCloudNoiseVolumes computes it, once, and the */               \
    /* renderer binds the images in the same order it numbers them. */                                                 \
    X( glm::vec4, SpeciesNoise, "CloudSpeciesNoise", {} )                                                              \
    /* THE MEDIUM'S SCATTERING ALBEDO, PER COLOUR. rgb; w = Sky Light Cloud Bottom Occlusion, which */                 \
    /* closes the hole the colour opened — Unreal's own arrangement of a scalar beside its albedo. */                  \
    /*  */                                                                                                             \
    /* WHY IT IS ITS OWN vec4 AND NOT THREE SLOTS SOMEWHERE. Three components of one colour have to be */              \
    /* contiguous to be fetched as `.rgb`, and no existing member had three free slots. A vec4 is */                   \
    /* therefore the smallest shape a colour fits in, and the fourth float is unreachable arithmetic */                \
    /* rather than a choice: this block is a grid of vec4s, so it can only grow by four, and turning a */              \
    /* one-float value into a three-float one asks for two. No shuffling of the other members removes */               \
    /* that — every candidate move (the species count, the AO strength into the albedo's alpha the way */              \
    /* Unreal packs it, the albedo inside Detail) closes one hole and opens another, because the */                    \
    /* remainder modulo four does not care where the values sit. */                                                    \
    /*  */                                                                                                             \
    /* AND IT SITS BEFORE Aerial, which closes the block; the species arrays sit with it for the same reason. */       \
    /* Every member is a vec4 or an array of them, so the generated block is a whole number of vec4s. */               \
    X( glm::vec4, Albedo, "CloudAlbedo", {} )                                                                          \
    /* THE WORLD WEATHER, as Assets::CloudFarWeatherUniform packs it: y the weather strength = PatchStrength */        \
    /* (ZERO when a painted pattern is the weather or the strength is nil), w 1 / kCloudFarWeatherPeriodKm; */         \
    /* x the far path's read scale of the same map (Assets::CloudFarFieldScale); z the Coverage slider, laid */        \
    /* over the uniform by PackCloudParams from the RESOLVED material (WTH-CLOUDS) — the march thresholds the */       \
    /* baked field by it (Assets::CloudProceduralCoverageRemap), so the weather moves it with no re-bake. */           \
    /* The march remaps the baked profile by the W this decides — Assets::CloudProceduralCoverRemap is the CPU */      \
    /* half of the same remap. */                                                                                      \
    X( glm::vec4, Weather, "CloudWeather", {} )                                                                        \
    /* THE WISPY BASE, PER SPECIES (x species 0 .. w species 3), in the LAYER's height fraction (the */                \
    /* march's own `heightFraction`): where the type's band starts and where its altitude density H */                 \
    /* first reaches its maximum (CloudProfileWispTopFraction). Between the two the march blends the */                \
    /* winner's DetailCharacter from 0 (wispy) up to the type's own — Nubis' wispy-at-the-base, */                     \
    /* billowy-above erosion, driven by the same curve the bake multiplies the profile by, so the base */              \
    /* that H thins is the base the erosion frays. Slots at or past the species count are zero. */                     \
    X( glm::vec4, SpeciesWispBase, "CloudSpeciesWispBase", {} )                                                        \
    X( glm::vec4, SpeciesWispTop, "CloudSpeciesWispTop", {} )                                                          \
    /* PER SPECIES (x species 0 .. w species 3), the material's CloudTypeWeight of the slot the species */             \
    /* came from (WTH-OVERCAST): the march cuts that species' channel at Coverage x this, so a weather fades */        \
    /* cumulus out and a stratus deck in without a re-bake. Slots at or past the species count are zero. */            \
    X( glm::vec4, SpeciesCover, "CloudSpeciesCover", {} )                                                              \
    /* THE PAINTED LAYOUT'S PLACE AND STRENGTH (WX-PAINT), Assets::CloudLayoutPlaceUniform / */                        \
    /* CloudLayoutStrengthUniform: the march multiplies the cover by the painted W per column, as Unreal's */          \
    /* Layout does, reading the CloudLayoutPatternTexture / CloudLayoutMaskTexture block entries through these. */     \
    X( glm::vec4, LayoutPlace, "CloudLayoutPlace", {} )                                                                \
    X( glm::vec4, LayoutStrength, "CloudLayoutStrength", {} )                                                          \
    /* THE FAR PATH (CLOUD-SEAM24), Assets::CloudProceduralFarStatistics of the coarsest clip level on the */          \
    /* device: per height band, per species channel, the share of the level's voxels that are cloud and */             \
    /* the mean profile where they are. Past that level's window the march draws a world column field */               \
    /* against these (Common/CloudField.glslh CloudSampleProceduralFar) instead of repeating its torus. */             \
    X( Vec4ArrayCloudGpuPayloadFarPresence, FarPresence, "CloudFarPresence", {} )                                      \
    X( Vec4ArrayCloudGpuPayloadFarProfile, FarProfile, "CloudFarProfile", {} )                                         \
    /* A vec4 AND LAST. Three values: the fourth slot is the std430 tail a generated block ends on (the */             \
    /* C++ size and the GLSL size are the same multiple of 16), registered in kCloudUnreadSlots. */                    \
    X( glm::vec4, Aerial, "CloudAerial", {} ) /* x AP far extent (km), y AP view-distance scale, z AP gate */
    DESERT_SHADER_STRUCT( CloudGpuPayload, "CloudGpuPayload", Std430, DESERT_CLOUD_GPU_PAYLOAD_FIELDS );
#undef DESERT_CLOUD_GPU_PAYLOAD_FIELDS

    inline constexpr ShaderProtocols::ShaderBlockDecl   kCloudParamsBlocks[] = { {
           .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding   = { ShaderProtocols::ShaderBindingKind::Overridable, kCloudParamsBinding, "CLOUD_PARAMS_BINDING" },
           .BlockName = "CloudParamsBuffer",
           .Members   = &kCloudGpuPayloadStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kCloudParamsShaderInclude{
         .Path         = "Generated/Clouds/CloudParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudLayout.hpp",
         .Blocks       = kCloudParamsBlocks,
    };

    /**
     * The temporal reconstruction's per-frame data (CloudResolveBuffer).
     *
     * A STORAGE BUFFER RATHER THAN A PUSH CONSTANT: two 4x4 matrices are already 128 bytes, the whole of what
     * Vulkan guarantees for push constants. The buffer is created NON-PERSISTENT, which gives it one copy per
     * (frame x recording renderer slot) — the Docs/RENDERER_FRAME_STATE.md rule.
     *
     * The vec3 leaves one float after it and HistoryValid occupies it; the int closes WindStepKm's sixteen bytes,
     * so the block ends on a multiple of 16 with no padding member.
     */
#define DESERT_CLOUD_RESOLVE_PARAMS_FIELDS( X )                                                                        \
    /* clip -> world, THIS frame, UNJITTERED (ViewFrame::InvViewProjection): rebuilds the pixel's ray. The */          \
    /* march traces through the same matrix (VolumetricCloudRenderer, SKY-FLICKER), so at a still camera */            \
    /* the reprojection through PrevViewProjection lands on this very texel. */                                        \
    X( glm::mat4, InverseViewProjection, "InverseViewProjection", {} )                                                 \
    /* world -> clip, the PREVIOUS frame. Composed with the inverse above this is Unreal's */                          \
    /* ClipToPrevClip; it is factored into two matrices here because the reprojected point is found by */              \
    /* walking the ray to the guide's cloud front distance, which is not a clip-space operation. */                    \
    X( glm::mat4, PrevViewProjection, "PreviousViewProjection", {} )                                                   \
    /* THE MARCH'S SHELL, CloudGpuPayload::Layer copied from the very payload the march was given this frame */        \
    /* (x planet radius, y bottom altitude, z thickness, w max view distance; km). The resolve classifies */           \
    /* each pixel's ray exactly as the march does (Common/CloudSceneDepth.glslh, CloudFootprintOccluded), */           \
    /* which takes the layer's segment along the ray. */                                                               \
    X( glm::vec4, CloudShell, "CloudShell", {} )                                                                       \
    X( glm::vec3, CameraPosition, "CameraPosition", {} ) /* world units (centimetres) */                               \
    X( float, HistoryValid, "HistoryValid", {} ) /* 1 when the history targets hold a real previous frame, 0 otherwise */ \
    X( glm::ivec2, SubPixelOffset, "SubPixelOffset", {} ) /* this frame's traced sub-pixel, the same pair the march was given */ \
    /* The march's reach along the ray, from the same payload: x the tracing start (CloudGpuPayload::March.z), */      \
    /* y the tracing start's maximum (CloudGpuPayload::Phase.w); km. */                                                \
    X( glm::vec2, CloudReach, "CloudReach", {} )                                                                       \
    /* THE WIND'S STEP SINCE THE HISTORY: this frame's CloudGpuPayload::Wind minus the one the view's previous */      \
    /* pair was traced under (VolumetricCloudHistory::Refs::WindOffsetKm), km in the field's frame. The resolve */     \
    /* reprojects each front from where the wind carried it from; zero when there is no history to read. */            \
    X( glm::vec3, WindStepKm, "WindStepKm", {} )                                                                       \
    /* This frame's phase of the trace grid's 2x2 lattice (CloudTraceGridPhase, as CloudTracePhase::Index), the */     \
    /* same one the march was given: a quarter-res texel holds a sample this frame only when */                        \
    /* (x & 1) | (y & 1) << 1 equals it. */                                                                            \
    X( int32_t, TraceGridPhase, "TraceGridPhase", {} )
    DESERT_SHADER_STRUCT( CloudResolveParams, "CloudResolveParams", Std430, DESERT_CLOUD_RESOLVE_PARAMS_FIELDS );
#undef DESERT_CLOUD_RESOLVE_PARAMS_FIELDS

    inline constexpr ShaderProtocols::ShaderBlockDecl   kCloudResolveParamsBlocks[] = { {
           .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding   = { ShaderProtocols::ShaderBindingKind::Fixed, kCloudResolveParamsBinding, {} },
           .BlockName = "CloudResolveBuffer",
           .Members   = &kCloudResolveParamsStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kCloudResolveParamsShaderInclude{
         .Path         = "Generated/Clouds/CloudResolveParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudLayout.hpp",
         .Blocks       = kCloudResolveParamsBlocks,
    };

    /// How many hero clouds one frame may carry: the generated CloudAuthoredPayload's array length, and
    /// CLOUD_AUTHORED_SLOTS in Common/CloudAuthored.glslh (Desert/Tests/Engine/CloudAuthored asserts the two agree).
    ///
    /// EIGHT, AND MEMORY IS WHAT PICKS IT — see the note beside CLOUD_AUTHORED_SLOTS. Eight DISTINCT
    /// bodies are 32.00 MiB of atlas; with the 20.67 MiB the subsystem already occupies that is 52.67 of
    /// decision D-9's 64 MiB, and the trace and history targets take 9.4 MiB more at 1920x1080 than at the
    /// 1280x766 that figure was measured at. Ten bodies would not leave room for the frame.
    inline constexpr uint32_t kCloudAuthoredSlots = 8u;

    /**
     * One hero cloud as the GPU reads it (GLSL CloudAuthoredInstance). TWENTY FLOATS, ALL READ — see the note
     * on the struct's use in Common/CloudAuthored.glslh for why the translation is not among them. KILOMETRES,
     * in the FIELD's frame: x and z are world kilometres, y is the altitude above the layer's base.
     */
#define DESERT_CLOUD_AUTHORED_INSTANCE_GPU_FIELDS( X )                                                                 \
    X( glm::vec4, Row0, "Row0", {} ) /* xyz = row 0 of field-km -> local cube, w = DetailFactor */                     \
    X( glm::vec4, Row1, "Row1", {} ) /* xyz = row 1,                          w = DensityFactor */                     \
    X( glm::vec4, Row2, "Row2", {} ) /* xyz = row 2,                          w = ExtinctionFactor */                  \
    X( glm::vec4, BoundsMin, "BoundsMin", {} ) /* xyz = field-space AABB min (km),      w = SIGNED Strength */         \
    X( glm::vec4, BoundsMax, "BoundsMax", {} ) /* xyz = field-space AABB max (km),      w = AtlasSlabBaseW */
    DESERT_SHADER_STRUCT( CloudAuthoredInstanceGpu, "CloudAuthoredInstance", Std430, DESERT_CLOUD_AUTHORED_INSTANCE_GPU_FIELDS );
#undef DESERT_CLOUD_AUTHORED_INSTANCE_GPU_FIELDS

    /**
     * The whole instance buffer (CloudAuthoredBuffer): how many instances are live and the instances themselves.
     *
     * THE TWO INTS AFTER THE COUNTS ARE std430's ALIGNMENT AND NOT A RESERVED SLOT. A struct array aligns to
     * 16, so the two counts are followed by eight bytes no value occupies; they are declared, on both sides,
     * so that the array's offset is the same declared fact for C++ and GLSL.
     *
     * A SEPARATE BUFFER FROM CloudGpuPayload: that one is the LAYER, repacked when the artist moves a slider;
     * this is a per-frame LIST whose length changes when an entity is added to the scene.
     */
    using Int32ArrayCloudAuthoredAlignment = std::array<int32_t, 2>;
    using CloudAuthoredInstanceArray       = std::array<CloudAuthoredInstanceGpu, kCloudAuthoredSlots>;
#define DESERT_CLOUD_AUTHORED_PAYLOAD_FIELDS( X )                                                                      \
    X( int32_t, Count, "CloudAuthoredCount", {} )                                                                      \
    /* How many DIFFERENT bodies the atlas bound this frame holds. Here and not on the instance because every */      \
    /* instance of a frame reads the same atlas: eight copies of one number are eight chances to disagree. */         \
    X( int32_t, SlabCount, "CloudAuthoredSlabCount", {} )                                                              \
    X( Int32ArrayCloudAuthoredAlignment, Alignment, "CloudAuthoredAlignment", {} )                                     \
    X( CloudAuthoredInstanceArray, Instances, "CloudAuthoredInstances", {} )
    DESERT_SHADER_STRUCT( CloudAuthoredPayload, "CloudAuthoredPayload", Std430, DESERT_CLOUD_AUTHORED_PAYLOAD_FIELDS );
#undef DESERT_CLOUD_AUTHORED_PAYLOAD_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kCloudAuthoredStructs[] = {
         &kCloudAuthoredInstanceGpuStruct, &kCloudAuthoredPayloadStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kCloudAuthoredShaderInclude{
         .Path         = "Generated/Clouds/CloudAuthored.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudLayout.hpp",
         .Structs      = kCloudAuthoredStructs,
    };

    /**
     * The cloud shadow map's consumer block: what every receiver of the cloud's shadow on the ground reads
     * (Common/CloudShadowReceiver.glslh). The programs own the slot — CloudShadowUB in the deferred lighting,
     * the forward and water surface passes, SurfaceCacheCloudShadowUB in the surface cache — and declare
     * `{ DESERT_CloudShadowUniforms_MEMBERS }`.
     *
     * EVERY SLOT OF `Params` IS A PROPERTY OF THE MAP, not a constant the reader could look up: the consumer
     * is told what it was handed. The strength is the artist's and applied at read time.
     */
#define DESERT_CLOUD_SHADOW_UNIFORMS_FIELDS( X )                                                                       \
    X( glm::mat4, WorldToMap, "CloudShadowWorldToMap", glm::mat4( 1.0f ) )                                             \
    /* x = the kilometres the clip z spans, y = 1 when the map is bound and must be read, z = the UV */                \
    /* width of the border fade, w = the artist's shadow strength. */                                                  \
    X( glm::vec4, Params, "CloudShadowParams", {} )
    DESERT_SHADER_STRUCT( CloudShadowUniforms, "CloudShadowUniforms", Both, DESERT_CLOUD_SHADOW_UNIFORMS_FIELDS );
#undef DESERT_CLOUD_SHADOW_UNIFORMS_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kCloudShadowStructs[] = { &kCloudShadowUniformsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kCloudShadowShaderInclude{
              .Path         = "Generated/Clouds/CloudShadow.glslh",
              .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudLayout.hpp",
              .Structs      = kCloudShadowStructs,
    };

    // The view march's binding for the block below (Programs/Clouds/CloudRaymarch.shader; its map is the
    // `Uniform(18)` entry beside it). Handed to SetStorageBuffer verbatim and written into the generated block.
    inline constexpr uint32_t kCloudSelfShadowBinding = 19u;

    /**
     * What the view march needs to read the cloud shadow map as its far shadow term (CloudSelfShadowBuffer).
     *
     * Map: x = the kilometres the map's clip z spans (CloudShadowMapView::FarDepthKm), y = 1 when the map
     *      was built this frame and must be read, z = the UV width of the border fade, w = the short ray's
     *      length in km (kCloudSelfShadowLocalKm).
     * LocalSamples: the short ray's sample count, the artist's LightMarchLocalSamples under the tier's ceiling.
     * Tail: the three floats that close the block on a multiple of 16 (a generated block is a whole number of
     *      its 16-byte alignment on both sides); nothing writes or reads them.
     *
     * THE ARTIST'S SHADOW STRENGTH IS NOT HERE: it is how dark the cloud's shadow on the GROUND is
     * (CloudShadowUniforms::Params.w). A cloud's shadow on itself is the medium's optical depth.
     */
    using FloatArrayCloudSelfShadowTail = std::array<float, 3>;
#define DESERT_CLOUD_SELF_SHADOW_PARAMS_FIELDS( X )                                                                    \
    X( glm::mat4, WorldToMap, "CloudSelfShadowWorldToMap", glm::mat4( 1.0f ) )                                         \
    X( glm::vec4, Map, "CloudSelfShadowMap", {} )                                                                      \
    X( float, LocalSamples, "CloudSelfShadowLocalSamples", 1.0f )                                                      \
    X( FloatArrayCloudSelfShadowTail, Tail, "CloudSelfShadowTail", {} )
    DESERT_SHADER_STRUCT( CloudSelfShadowParams, "CloudSelfShadowParams", Std430,
                          DESERT_CLOUD_SELF_SHADOW_PARAMS_FIELDS );
#undef DESERT_CLOUD_SELF_SHADOW_PARAMS_FIELDS

    inline constexpr ShaderProtocols::ShaderBlockDecl   kCloudSelfShadowBlocks[] = { {
           .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
           .Binding   = { ShaderProtocols::ShaderBindingKind::Fixed, kCloudSelfShadowBinding, {} },
           .BlockName = "CloudSelfShadowBuffer",
           .Members   = &kCloudSelfShadowParamsStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kCloudSelfShadowShaderInclude{
         .Path         = "Generated/Clouds/CloudSelfShadow.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Clouds/CloudLayout.hpp",
         .Blocks       = kCloudSelfShadowBlocks,
    };
} // namespace Desert::Graphic

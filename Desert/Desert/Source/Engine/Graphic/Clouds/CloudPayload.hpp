#pragma once

#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * The GPU side of VolumetricCloudData, and the ONLY place the component is turned into bytes.
     *
     * The GLSL half of this layout is the block in Editor/Resources/Shaders/Common/CloudParams.glslh,
     * member for member and in this order. The static_asserts below make a divergence a build error
     * instead of a frame in which every parameter after the inserted one is read from the wrong offset —
     * which does not look like a bug, it looks like the clouds being badly tuned.
     *
     * Units: KILOMETRES throughout. This packer is where the component's world-unit distances are
     * converted, exactly once.
     *
     * EVERY SLOT IS READ, OR IT HAS A ROW IN kUnreadSlots SAYING WHY NOT. The block is 71 floats with no
     * reserved field and no padding, and that is a deliberate constraint rather than an accident of
     * packing: a spare slot is where a future parameter gets quietly stashed without a name, a range or a
     * tooltip. Two of the 71 are not read, both of them the toll std430 charges for storing a
     * three-component COLOUR in a grid of vec4s — see kUnreadSlots below, which is the register a test
     * checks the shader text against. It is a register with a reason per row and not a count: a count is
     * the number the next author adjusts instead of explaining.
     *
     * TWENTY OF THE SEVENTY-ONE ARE THE FOUR SPECIES' — SpeciesEdge's sixteen and SpeciesNoise's four. The
     * three slots T3 freed by moving a type's factors out of the layer's own vec4s were spent rather than
     * kept: `Weather.w` held the one species' DetailCharacter, `Detail.y` and `Detail.z` held the products
     * of the layer's strength and density with that species' factors, and `March.w` held the product with
     * its extinction. With four kinds of cloud in one shell none of those products can be formed once, so
     * each became a per-species number and the vec4s they left closed up around the layer's own settings.
     *
     * THE PREVIOUS SENTENCE READ "THIRTY-TWO OF THE SEVENTY-NINE" UNTIL 2026-09-08, AND BOTH NUMBERS WERE
     * ONCE TRUE. 79 floats is 316 bytes: this block BEFORE the per-species placement basis left it (four
     * vec4s, -64 bytes) and before SpeciesNoise arrived (+16). Thirty-two was that basis's sixteen floats
     * plus SpeciesEdge's sixteen. So the sentence was not a typo and nobody miscounted — it was ORPHANED
     * from the layout by two changes that did not come back for it, which is a different defect from a
     * wrong comment and needs a different habit: whoever moves a layout owns every number written about
     * it. O1_DESIGN.md §8.2 recorded this as a numeric typo; it was not, and §10.5 records what it was.
     */
    struct CloudGpuPayload
    {
        glm::vec4 Layer;        // x planet radius, y bottom altitude, z thickness, w max view distance (km)
        // w is the LAYER's extinction alone; the winning species' factor multiplies it in the march.
        glm::vec4 March;        // x max steps, y stop transmittance, z tracing start (km), w extinction (1/km)
        // THE MODELLING VOLUME'S REGION, IN THE SLOT THE WEATHER SETTINGS VACATED.
        //
        // This vec4 held (coverage tile, coverage, coverage contrast, detail tile). Three of those four
        // decided WHERE cloud is, and none of them decides it any more: the lattice a cluster of lumps is
        // drawn in, the fraction of cells that carry one, and how sharply a cell fills are all consumed by
        // Assets::BakeCloudProceduralVolume on the CPU, at bake time. Sending them to the march as well
        // would be three dead slots in a block whose whole discipline is that every slot has a consumer —
        // so the region moved in and the fourth number, which the erosion still reads, stayed put.
        //
        // xy the region's minimum corner in world kilometres, SNAPPED to the lump lattice by
        // Assets::CloudProceduralRegionOriginKm; z the RECIPROCAL of its horizontal side, because the march
        // only ever divides by it; w the erosion's period, unchanged.
        glm::vec4 Region;
        // z HELD THE SCALAR SCATTERING ALBEDO and holds nothing now — the albedo became a colour and moved
        // to its own vec4 below. It is not reused and not renamed to a pad: kUnreadSlots carries its row.
        glm::vec4 Detail;       // x detail strength, y density scale, z UNREAD, w species count
        glm::vec4 Wind;         // xyz accumulated wind offset (km), w phase g
        glm::vec4 Sun;          // xyz TOWARD the sun (normalized), w light march distance (km)
        glm::vec4 SunColour;    // rgb sun irradiance (linear), w light march sample count
        glm::vec4 Ambient;      // rgb ambient radiance OR its scale, w = 1 when the distant sky light is read
        glm::vec4 MultiScatter; // x octave count, y contribution, z occlusion, w eccentricity
        glm::vec4 Phase;        // x second lobe g, y phase blend, z AO strength, w tracing start max (km)
        glm::vec4 Fade;         // x AP start (km), y AP fade (km), z near fade end (km), w near fade start (km)

        // THE FOUR SPECIES. Both arrays are indexed by the same slot, and slots at or past Detail.w are
        // zero — the loop in the march stops at the count, and an unfilled slot's channel of the profile
        // table is zero at every altitude anyway, so there are two independent reasons it is never read.
        //
        // x DetailCharacter, y DetailFactor, z DensityFactor, w ExtinctionFactor: what that kind of cloud
        // is MADE OF. The winner of the union at a sample takes all four; nothing is averaged.
        glm::vec4 SpeciesEdge[kCloudSpeciesSlots];

        // WHICH NOISE VOLUME EACH SPECIES' EDGE IS CUT FROM — x for species 0, y for 1, z for 2, w for 3,
        // each of them an index into the four `sampler3D` slots the march binds. Four floats and four
        // consumers, which is the same discipline the array above is held to.
        //
        // IT IS A DEDUPLICATED INDEX AND NOT THE SPECIES NUMBER, and the difference is the whole reason
        // this vec4 exists rather than the march simply reading `sampler[speciesIndex]`. Eight of the nine
        // shipped types name no volume of their own, so the ordinary four-species sky reads ONE volume
        // through four descriptors — and a fetch selected by a value that varies across a wave costs the
        // wave every branch it takes, whether or not the branches land on the same image. Deduplicated,
        // that sky sends {0,0,0,0}, the compare chain is uniformly false, and the march does exactly what
        // it did before this field existed. Graphic::ResolveCloudNoiseVolumes computes it, once, and the
        // renderer binds the images in the same order it numbers them.
        glm::vec4 SpeciesNoise;

        // THE MEDIUM'S SCATTERING ALBEDO, PER COLOUR. rgb; w is not read (kUnreadSlots).
        //
        // WHY IT IS ITS OWN vec4 AND NOT THREE SLOTS SOMEWHERE. Three components of one colour have to be
        // contiguous to be fetched as `.rgb`, and no existing member had three free slots. A vec4 is
        // therefore the smallest shape a colour fits in, and the fourth float is unreachable arithmetic
        // rather than a choice: this block is a grid of vec4s, so it can only grow by four, and turning a
        // one-float value into a three-float one asks for two. No shuffling of the other members removes
        // that — every candidate move (the species count, the AO strength into the albedo's alpha the way
        // Unreal packs it, the albedo inside Detail) closes one hole and opens another, because the
        // remainder modulo four does not care where the values sit.
        //
        // AND IT SITS BEFORE THE TRAILING vec3, WHICH IS NOT A PREFERENCE. `glm::vec3` aligns to 4 and a
        // std430 vec4 aligns to 16, so a vec4 appended AFTER Aerial would land at 268 in C++ and at 272 in
        // GLSL — a silent divergence of the two halves of one layout, which is the worst outcome this file
        // exists to prevent. The species array is where it is for the same reason; see the note at its own
        // static_assert.
        glm::vec4 Albedo;

        // A vec3 AND LAST, which is the only shape in which three values can be three values. It was a
        // vec4 whose fourth slot carried the cloud type's variance, and then briefly the domain warp's
        // amount; the warp was measured and taken out again (Common/CloudField.glslh has the numbers), and
        // a float nobody reads is where the next parameter gets stashed without a name, a range or a
        // tooltip. Moving it to the end is what lets it shrink: std430 aligns it to 16 and the block ends
        // at 268 bytes rather than at 272.
        glm::vec3 Aerial; // x AP far extent (km), y AP view-distance scale, z AP gate
    };

    static_assert( offsetof( CloudGpuPayload, Layer ) == 0 );
    static_assert( offsetof( CloudGpuPayload, March ) == 16 );
    static_assert( offsetof( CloudGpuPayload, Region ) == 32 );
    static_assert( offsetof( CloudGpuPayload, Detail ) == 48 );
    static_assert( offsetof( CloudGpuPayload, Wind ) == 64 );
    static_assert( offsetof( CloudGpuPayload, Sun ) == 80 );
    static_assert( offsetof( CloudGpuPayload, SunColour ) == 96 );
    static_assert( offsetof( CloudGpuPayload, Ambient ) == 112 );
    static_assert( offsetof( CloudGpuPayload, MultiScatter ) == 128 );
    static_assert( offsetof( CloudGpuPayload, Phase ) == 144 );
    static_assert( offsetof( CloudGpuPayload, Fade ) == 160 );
    // The species array sits BEFORE the trailing vec3 rather than after it, and that is the only reason it
    // is there rather than appended. std430 aligns an array of vec4 to 16; appended after a vec3 that ends
    // at 188 it would start at 192 and leave four bytes nobody wrote — which is a reserved slot with extra
    // steps. Both sides of the layout move together in one commit and the offsets below are what makes a
    // half-move a build error rather than a frame read from the wrong place.
    static_assert( offsetof( CloudGpuPayload, SpeciesEdge ) == 176 );
    // The noise index array sits between the species array and the trailing vec3 for the reason the
    // species array itself sits there: std430 aligns a vec4 to 16, and after a vec3 that ends at 252 it
    // would start at 256 and leave four bytes nobody wrote.
    static_assert( offsetof( CloudGpuPayload, SpeciesNoise ) == 240 );
    static_assert( offsetof( CloudGpuPayload, Albedo ) == 256 );
    static_assert( offsetof( CloudGpuPayload, Aerial ) == 272 );
    // 284, NOT 288, and the difference is the point. std430 rounds a block's STRIDE up to a multiple of
    // 16, but a stride only exists for an ARRAY of blocks and this is a single one — the shader never
    // reads past the last member, so the block ends at 284 and so does this. glm::vec3 aligns to 4 rather
    // than to 16, so the C++ struct ends there too and there is no trailing padding to explain. Same
    // arrangement, and the same reasoning, as CloudResolveParams below.
    //
    // THE BLOCK SHRANK BY 64 BYTES, which is the four vec4s of the per-species placement basis. Nothing
    // was appended to replace them: the region took the weather settings' slot, and the settings that
    // moved to the bake left the block rather than travelling to a march that would not read them.
    //
    // AND IT GREW BY SIXTEEN TWICE. Once for SpeciesNoise — the price of a type's noise volume reaching
    // the march at all; until it was paid, three of a layer's four slots could name a volume the frame
    // never read. Once for Albedo, which is the price of the scattering albedo being a COLOUR: a vec4 is
    // the smallest shape three contiguous components fit in.
    static_assert( sizeof( CloudGpuPayload ) == 284,
                   "Eleven vec4s, a vec4[4], two more vec4s and a vec3 — the shader reads exactly this and "
                   "nothing more." );

    inline constexpr uint32_t kCloudPayloadBytes = sizeof( CloudGpuPayload );

    /// How many floats travel in the block. DERIVED from the struct, so it cannot be a number somebody
    /// updates separately from the layout — which is exactly what happened to the two prose counts above.
    inline constexpr uint32_t kCloudPayloadFloats = kCloudPayloadBytes / 4u;

    /// ONE SLOT OF THE BLOCK THAT NO SHADER READS, AND WHY.
    ///
    /// The block's discipline is that a slot nobody reads is a dead setting wearing a parameter's clothes.
    /// std430 makes that unachievable the moment a value becomes a three-component COLOUR: the block is a
    /// grid of vec4s, so it can only grow by four, and turning one float into three asks for two.
    ///
    /// SO THE EXCEPTION IS REGISTERED PER SLOT, WITH A REASON, AND NOT COUNTED. A count is the number the
    /// next author adjusts instead of explaining; a register makes a third unread slot red BECAUSE IT HAS
    /// NO ROW. Desert/Tests/Engine/CloudMediumConsumers reads the shader tree, works out which components
    /// of which member are actually fetched, and requires every unfetched one to appear here.
    struct CloudUnreadSlot
    {
        /// The member's GLSL name, exactly as the block declares it (`u_CloudDetail`).
        const char* Member;
        /// Which component, as the single swizzle letter a shader would fetch it by: 'x', 'y', 'z' or 'w'.
        char Component;
        /// Why nothing reads it. A row with an empty reason is not a row.
        const char* Reason;
    };

    /// std::array AND NOT A C ARRAY, because this register is allowed to reach zero rows — the day the
    /// block's arithmetic works out even again, every exception goes away, and a zero-length C array is
    /// not a thing in C++: clang takes it as a GNU extension and MSVC rejects it outright (C2466). That
    /// has reached `dev` twice in one day from two censuses that achieved their own goal. A type has to be
    /// able to express its structure's success.
    inline constexpr std::array<CloudUnreadSlot, 2> kCloudUnreadSlots = {
         { { "u_CloudDetail", 'z',
             "held the scalar scattering albedo until the albedo became a colour and moved to "
             "u_CloudAlbedo; not reused, because a slot repurposed without a schema parameter behind it is "
             "how a value reaches the GPU with no name, no range and no tooltip" },
           { "u_CloudAlbedo", 'w',
             "the toll std430 charges for a three-component colour: a grid of vec4s can only grow by four, "
             "and a colour needs three contiguous components, so the fourth is unreachable arithmetic "
             "rather than a reserved field" } } };

    /// How many of the block's floats a shader is expected to fetch. DERIVED, so the two halves of the
    /// claim — the layout and the exceptions — cannot be adjusted independently.
    inline constexpr uint32_t kCloudPayloadReadFloats =
         kCloudPayloadFloats - static_cast<uint32_t>( kCloudUnreadSlots.size() );

    // The bindings of the cloud compute pass. SetOutput / SetStorageBuffer / SetInput take these as
    // explicit arguments and never consult the shader's own reflection, so each number here must equal
    // the one written in the shader. A mismatch lands a resource on a different descriptor rather than on
    // an error.
    inline constexpr uint32_t kCloudOutputBinding     = 0; // the RGBA16F scatter image the march writes
    inline constexpr uint32_t kCloudParamsBinding     = 1; // must equal CLOUD_PARAMS_BINDING
    inline constexpr uint32_t kCloudSceneDepthBinding = 2;
    inline constexpr uint32_t kCloudNoiseBinding      = 3;
    // The sky's DISTANT SKY LIGHT. ALWAYS bound, even in a scene with no physical atmosphere: a declared
    // sampler with no image is an invalid descriptor set, not an unused one, and this engine's compute
    // path answers an invalid set by skipping the dispatch entirely — the clouds would vanish with
    // nothing in the log. What varies is CloudGpuPayload::Ambient.w, which is 0 when there is no texel.
    inline constexpr uint32_t kCloudDistantSkyLightBinding = 4;
    // The sky's camera aerial-perspective volume, which the march composes distant cloud INTO. Bound on
    // the same terms as the texel above: always, gated by CloudGpuPayload::Aerial.z.
    inline constexpr uint32_t kCloudAerialPerspectiveBinding = 5;
    // The march's SECOND output: the depth guide the composite upsamples through (front cloud distance
    // and scene distance, kilometres). A storage image like binding 0, not a sampler, so it is bound with
    // SetOutput and lives in the same GENERAL-layout round trip as the scatter target.
    inline constexpr uint32_t kCloudGuideOutputBinding = 6;
    // The procedural MODELLING VOLUME (Engine/Assets/CloudProceduralVolume.hpp): 256 x 32 x 256 RGBA8,
    // channel k being species k's Dimensional Profile. Owned by Runtime::CloudProceduralVolumeService and
    // re-baked only when the layer's settings change or the camera crosses a snap of the lump lattice.
    //
    // IT KEPT THE NUMBER THE PROFILE TABLE HAD, which is worth saying rather than leaving to be noticed:
    // one binding went out and one came in, so the descriptor set's shape is unchanged and every
    // consumer's slot list stayed where it was.
    inline constexpr uint32_t kCloudModellingBinding = 7;
    // THE SKY-LIGHT OCCLUSION VOLUME (Engine/Graphic/Clouds/CloudSkyOcclusionPayload.hpp): 128 x 16 x 128
    // RGBA16F over the modelling volume's own region, .r holding the diffuse transmittance of the cloud
    // ABOVE that column at that altitude. Bound on the same terms as the distant sky light and the aerial
    // perspective volume — ALWAYS, fallback included, gated by CloudPush::Frame.x — because a
    // declared sampler with no image is an invalid descriptor set and this backend answers one by skipping
    // the dispatch. 13 rather than 8: the authored buffer and its atlas took 8 and 9, and noise volumes 1
    // to 3 took 10 to 12.
    inline constexpr uint32_t kCloudSkyOcclusionBinding = 13;
    // THE ATMOSPHERE'S TRANSMITTANCE LUT (Programs/Sky/SkyTransmittanceLut.shader): 256x64 of what
    // survives the trip from a point in the atmosphere to space, in Bruneton's (r, mu) mapping. Read only
    // when CloudPush::Frame.y is 1, i.e. when the layer asked for per-sample atmospheric sun transmittance
    // AND the sky actually baked the pair.
    //
    // ALWAYS BOUND, fallback included, on the terms every sampler above is bound on: a declared sampler
    // with no image is an INVALID descriptor set rather than an unused one, and this backend answers one
    // by skipping the whole dispatch — every cloud in the frame would disappear with nothing in the log.
    //
    // THE ENVIRONMENT BAKE NEEDS NO NUMBER OF ITS OWN. Programs/Compute/BakeProceduralSky.shader marches
    // the same field and already binds the same LUT as the SKY's own resource
    // (Graphic::kSkyTransmittanceLutBinding), so it applies this feature through that descriptor and only
    // its gate travels — see CloudBakeBinding::PerSampleSunTransmittance.
    inline constexpr uint32_t kCloudSunTransmittanceLutBinding = 14;

    /**
     * @brief The FOUR noise volumes a layer can bind, by descriptor number, in the order CloudGpuPayload::
     *        SpeciesNoise indexes them.
     *
     * SEPARATE BINDINGS AND NOT AN ARRAY OF SAMPLERS, and the reason is not preference: this engine's
     * reflection refuses one in so many words — VulkanShaderReflection.cpp, "arrays of descriptors are not
     * supported — declare separate bindings" — because every layout it builds hardcodes descriptorCount 1.
     * Common/CloudAuthored.glslh met the same wall and answered it with an ATLAS, which is the better
     * answer THERE and the wrong one here: an atlas is addressed by arithmetic on a clamped coordinate,
     * and the erosion's coordinate is unbounded and relies on the sampler's own REPEAT to tile. Stacking
     * the volumes would put the neighbour's texels under every wrap, which is a seam in the sky at every
     * period of the erosion rather than a saving.
     *
     * FOUR AND NOT MORE, because a layer has four species and therefore at most four distinct volumes.
     * THEY COST ONLY WHAT IS NAMED: a `.dcnv` is read and uploaded when a layer names it (AL1-2), so the
     * four descriptors point at volumes the scene asked for, never at the whole project's.
     *
     * SLOT 0 KEEPS BINDING 3 so that the number the whole subsystem has always meant by "the cloud noise"
     * is unchanged, and the three that follow take the first free numbers after the authored atlas.
     */
    inline constexpr uint32_t kCloudNoiseBindings[kCloudSpeciesSlots] = { kCloudNoiseBinding, 10, 11, 12 };

    // The TEMPORAL RESOLVE pass (Programs/Clouds/CloudTemporalResolve.shader). Same rule as above: these
    // numbers are handed to SetInput / SetOutput / SetStorageBuffer verbatim and must equal the ones
    // written in the shader.
    inline constexpr uint32_t kCloudResolveTraceBinding        = 0; // quarter-res scatter, this frame
    inline constexpr uint32_t kCloudResolveTraceGuideBinding   = 1; // quarter-res guide, this frame
    inline constexpr uint32_t kCloudResolveHistoryBinding      = 2; // half-res scatter, previous frame
    inline constexpr uint32_t kCloudResolveHistoryGuideBinding = 3; // half-res guide, previous frame
    inline constexpr uint32_t kCloudResolveOutputBinding       = 4; // half-res reconstructed scatter
    inline constexpr uint32_t kCloudResolveGuideOutputBinding  = 5; // half-res reconstructed guide
    inline constexpr uint32_t kCloudResolveParamsBinding       = 6; // CloudResolveParams

    /**
     * Per-dispatch data: everything that changes with the CAMERA rather than with the cloud settings.
     * Rides in the push constant because ComputePipeline has no SetUniformBuffer, and a second storage
     * buffer for one matrix would be a second per-frame upload.
     */
    struct CloudPush
    {
        glm::mat4 InverseViewProjection;
        // xyz = camera position in world units; w = the frame index, which decides the dither pattern.
        // It is here rather than in the parameter block because it changes every frame while the block
        // changes only when the artist does something.
        glm::vec4 CameraPosition;
        // x, y = the sub-pixel this frame traces inside its 2x2 block of HALF-resolution pixels, each 0
        //        or 1. This is the projection jitter: the march reconstructs its ray through the HALF-res
        //        pixel (traceCoord * 2 + offset), not through the centre of its own quarter-res texel.
        // z, w = the HALF-resolution grid's size in pixels. The march writes a quarter-res image, so
        //        imageSize() cannot tell it the grid its jitter is expressed in, and half-of-half rounds
        //        UP twice — deriving it from the quarter size would be off by a pixel on odd viewports and
        //        would shear the whole jitter pattern by half a texel along the right and bottom edges.
        glm::vec4 Trace;
        // WHAT THIS FRAME'S RESOURCES ACTUALLY ARE, as opposed to what the artist asked for. Every lane
        // here is a property of the FRAME and not of the weather, which is why they are in the push
        // constant rather than in the parameter block beside the artist's fields: a component flag can be
        // on while the resource it names does not exist (no atmosphere, a failed allocation, a bake that
        // has not run), and a march that read an image nobody wrote would shade the sky with uninitialised
        // device memory.
        //
        // x = 1 when the sky-light occlusion volume was dispatched for THIS frame and must be read.
        //     A GATE AND NOT A REGION, which is the whole reason the volume costs one float here: it
        //     shares the procedural modelling volume's frame exactly, so its origin and side are already
        //     on the wire as CloudGpuPayload::Region and a copy of them here would be the same two numbers
        //     travelling twice.
        // y = 1 when the atmosphere's transmittance LUT was baked for THIS frame AND the layer asked for
        //     per-sample atmospheric sun transmittance. When it is 1, CloudGpuPayload::SunColour carries
        //     the sun's OUTER-SPACE illuminance and the march applies T(sample) itself; when it is 0 the
        //     same field carries the ground-level product and the march applies nothing. The two always
        //     agree because Graphic::CloudUsesPerSampleSunTransmittance decides both.
        // z, w = the atmosphere shell's bottom and top radii, kilometres from the planet centre — the
        //     domain of the LUT's Bruneton mapping, which its reader has to reproduce exactly. They come
        //     from AtmosphereEnv rather than from the cloud component for the reason the aerial
        //     perspective's two scalars do: they describe the SKY's resource, and a second authored copy
        //     is how a fill and a read end up disagreeing about a mapping. Written unconditionally and
        //     read only when y is 1.
        //
        // THE MEMBER USED TO BE CALLED SkyOcclusion and used to carry one lane. Renaming it is what keeps
        // the other three from being three unnamed floats hiding under a name that describes the first.
        glm::vec4 Frame;
    };

    static_assert( sizeof( CloudPush ) == 112,
                   "CloudPush must stay inside the 128 bytes Vulkan guarantees for push constants" );

    /**
     * Unreal's VolumetricRenderTarget sub-pixel walk, mode 0.
     *
     * Four frames cover the four half-resolution pixels of one quarter-resolution texel, in the order
     * {0, 2, 3, 1} rather than {0, 1, 2, 3}.
     *
     * WHY THAT ORDER. It is the 2x2 BAYER MATRIX read row by row — [[0, 2], [3, 1]] — so the four frames
     * visit the block in the ordered-dithering sequence rather than in raster order. Epic's own name for
     * the table says exactly this and is the only justification Epic gives: VolumetricRenderTarget.cpp:308
     * declares it as `OrderDithering2x2`, and the neighbouring branch at :314 carries
     * {0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5} — the canonical 4x4 Bayer matrix — for its own
     * downsample factor. One family of tables, one rule, two sizes.
     *
     * A raster order would spend two consecutive frames on the top row before touching the bottom one,
     * which reads as a horizontal crawl at exactly the frequency the eye is best at seeing; the Bayer
     * order is the standard answer to that and spreads each successive sample as far as it can from the
     * ones already taken.
     *
     * (The comment this replaces claimed the order puts consecutive frames DIAGONALLY opposite each
     * other. It does not, and no order could: unrolled through the formula below the walk is
     * (0,0) -> (0,1) -> (1,1) -> (1,0) -> (0,0), which is the four SIDES of the square, and a diagonal
     * 4-cycle on a 2x2 block does not exist at all because the two diagonals are disjoint pairs. The
     * table was right and its stated reason was not, which is worse than no reason — the reason is what
     * the next person will decide by.)
     *
     * Pure and constexpr, so it can be evaluated where it is needed rather than cached into a member
     * that can disagree with the frame counter beside it.
     */
    struct CloudSubPixel
    {
        uint32_t X;
        uint32_t Y;
    };

    inline constexpr uint32_t kCloudTraceSubPixelCount = 4;

    inline constexpr CloudSubPixel CloudTraceSubPixel( uint32_t frameIndex )
    {
        constexpr uint32_t order[kCloudTraceSubPixelCount] = { 0u, 2u, 3u, 1u };

        const uint32_t index = order[frameIndex % kCloudTraceSubPixelCount];
        return CloudSubPixel{ index % 2u, index / 2u };
    }

    static_assert( CloudTraceSubPixel( 0u ).X == 0u && CloudTraceSubPixel( 0u ).Y == 0u );
    static_assert( CloudTraceSubPixel( 1u ).X == 0u && CloudTraceSubPixel( 1u ).Y == 1u );
    static_assert( CloudTraceSubPixel( 2u ).X == 1u && CloudTraceSubPixel( 2u ).Y == 1u );
    static_assert( CloudTraceSubPixel( 3u ).X == 1u && CloudTraceSubPixel( 3u ).Y == 0u );
    static_assert( CloudTraceSubPixel( 4u ).X == CloudTraceSubPixel( 0u ).X, "The walk must be periodic." );

    /**
     * The temporal reconstruction's per-frame data.
     *
     * A STORAGE BUFFER RATHER THAN A PUSH CONSTANT, and the reason is a hard limit rather than a
     * preference: this pass needs two 4x4 matrices (128 bytes) plus the camera and the frame's state, and
     * Vulkan only guarantees 128 bytes of push-constant space. Several desktop drivers report exactly
     * that minimum, so a 160-byte push block is a pass that works on this machine and fails elsewhere
     * with a validation error rather than a picture. The buffer is created NON-PERSISTENT, which is what
     * gives it one copy per (frame x recording renderer slot) — the Docs/RENDERER_FRAME_STATE.md rule.
     *
     * Laid out for std430 with no padding member: the vec3 leaves exactly one float of room after it and
     * HistoryValid occupies it, which is why the flag is a float and sits where it does.
     */
    struct CloudResolveParams
    {
        glm::mat4 InverseViewProjection; // clip -> world, THIS frame; rebuilds the pixel's ray
        // world -> clip, the PREVIOUS frame. Composed with the inverse above this is Unreal's
        // ClipToPrevClip; it is factored into two matrices here because the reprojected point is found by
        // walking the ray to the guide's cloud front distance, which is not a clip-space operation.
        glm::mat4  PrevViewProjection;
        glm::vec3  CameraPosition; // world units (centimetres)
        float      HistoryValid;   // 1 when the history targets hold a real previous frame, 0 otherwise
        glm::ivec2 SubPixelOffset; // this frame's traced sub-pixel, the same pair the march was given
    };

    static_assert( offsetof( CloudResolveParams, InverseViewProjection ) == 0 );
    static_assert( offsetof( CloudResolveParams, PrevViewProjection ) == 64 );
    static_assert( offsetof( CloudResolveParams, CameraPosition ) == 128 );
    static_assert( offsetof( CloudResolveParams, HistoryValid ) == 140 );
    static_assert( offsetof( CloudResolveParams, SubPixelOffset ) == 144 );
    // 152, not 160. std430 rounds a block's STRIDE up to a multiple of 16, but a stride only exists for an
    // array of blocks and this is a single one — the shader never reads past the last member, so the
    // buffer is created at exactly the struct's size and the eight bytes of tail padding are not
    // allocated. Pinned here so that inserting a member and quietly changing the size is a build error.
    static_assert( sizeof( CloudResolveParams ) == 152,
                   "The GLSL block in CloudTemporalResolve.shader reads exactly these members, in this "
                   "order, at these offsets." );

    inline constexpr uint32_t kCloudResolveParamsBytes = sizeof( CloudResolveParams );

    // One world unit is one centimetre (Common::Units), so a kilometre is 100 000 of them. Mirrored by
    // CLOUD_WORLD_UNITS_PER_KM in Common/CloudGeometry.glslh.
    inline constexpr float kCloudWorldUnitsPerKm = 100000.0f;

    // ---------------------------------------------------------------------------------------------
    // THE MARCH'S SEARCH RESOLUTION, ON THE C++ SIDE
    // ---------------------------------------------------------------------------------------------
    //
    // Mirrored from CLOUD_DISTANCE_TO_MAX_STEPS_KM and CLOUD_COARSE_STEP_MULTIPLIER in
    // Common/CloudGeometry.glslh, and Desert/Tests/Engine/CloudGeometry asserts the pair agrees with the
    // shader's own — which it can, because that suite compiles the header as C++. Mirroring rather than
    // including is the arrangement kCloudWorldUnitsPerKm above already records, and for the same reason:
    // the engine does not put the shader root on its include path.
    //
    // WHY THE ENGINE NEEDS IT AT ALL, which it did not before phase Э5. The procedural producer's lumps
    // are now placed on the CPU, and how small a lump may be is decided by what the march can FIND rather
    // than by what it can integrate: outside cloud the ray strides by the coarse step and only drops to
    // the fine tier once a coarse sample has already found material, so a body that fits between two
    // coarse samples is never seen and whether it fits is decided by the ray's jitter. The generator
    // clamps against this, so lowering Max Steps buys coarser clouds instead of speckle.
    inline constexpr float kCloudDistanceToMaxStepsKm = 4.0f;
    inline constexpr float kCloudCoarseStepMultiplier = 4.0f;

    /// The finest chord the schedule can resolve anywhere, kilometres. @p maxCount is the component's
    /// Max Steps. Two coarse steps — Nyquist against the SEARCH lattice, not the integration one.
    inline float CloudFinestResolvableChordKm( float maxCount )
    {
        const float fineStepKm = kCloudDistanceToMaxStepsKm / std::max( maxCount, 1.0f );
        return 2.0f * kCloudCoarseStepMultiplier * fineStepKm;
    }

    /**
     * The near-camera fade's interval, in kilometres and in the order the shader consumes it.
     *
     * WHY THIS IS A PAIR AND NOT TWO NUMBERS. The march evaluates `smoothstep(start, end, t)`, and GLSL
     * leaves smoothstep UNDEFINED when start >= end: at start > end the ratio is negative before the
     * clamp on some implementations and not on others, and at start == end it is a division by zero.
     * Repairing each field on its own — which is what every other line of the packer does, and correctly
     * — cannot reach this, because both `Start = 5 km` and `End = 1 km` are individually legal, inside
     * their own sliders, and one edit apart in the Details panel. The RELATION is the thing that has to
     * be repaired, so it is repaired once, here, where the component becomes bytes.
     *
     * WHAT A CONTRADICTORY PAIR MEANS: nothing, so it is answered with OFF rather than with a guess. An
     * interval whose end does not lie past its start is not a fade over some other interval — reordering
     * the two, or nudging the end past the start, would put a distance nobody authored into a frame
     * nobody can explain, and that is the failure mode this programme has paid for most often. OFF is
     * also the shipped state: both fields default to zero, so the disabled interval is the one every
     * scene already carries.
     *
     * The shader's gate (`u_CloudFade.z > 0`) is exactly equivalent to `end > start` under this
     * guarantee, because the only pair with a zero end this returns is the pair with a zero start.
     */
    struct CloudNearFadeKm
    {
        float StartKm;
        float EndKm;
    };

    inline constexpr CloudNearFadeKm CloudResolveNearFade( float startWorld, float endWorld )
    {
        const float startKm = ( startWorld > 0.0f ? startWorld : 0.0f ) / kCloudWorldUnitsPerKm;
        const float endKm   = ( endWorld > 0.0f ? endWorld : 0.0f ) / kCloudWorldUnitsPerKm;

        if ( !( endKm > startKm ) )
            return CloudNearFadeKm{ 0.0f, 0.0f };

        return CloudNearFadeKm{ startKm, endKm };
    }

    // OFF is a fixed point: feeding the disabled pair back in leaves it disabled, so a scene that has been
    // through the packer once cannot acquire a fade by being saved and loaded.
    static_assert( CloudResolveNearFade( 0.0f, 0.0f ).EndKm == 0.0f );
    // The contradiction the review found, and the one the sliders make reachable in a single edit.
    static_assert( CloudResolveNearFade( 500000.0f, 100000.0f ).EndKm == 0.0f );
    // A degenerate interval of zero width is a division by zero in the shader, not a fade of zero length.
    static_assert( CloudResolveNearFade( 100000.0f, 100000.0f ).EndKm == 0.0f );
    // And a legal pair survives untouched, in kilometres.
    static_assert( CloudResolveNearFade( 100000.0f, 500000.0f ).StartKm == 1.0f );
    static_assert( CloudResolveNearFade( 100000.0f, 500000.0f ).EndKm == 5.0f );

    /**
     * Fill the GPU block from the component, the atmosphere and this frame's accumulated wind offset.
     * Pure: no GPU, no globals, no clock — the CloudPayload tests drive it directly.
     *
     * @param shapes          the cloud types in the layer's slots, packed to the front, already resolved
     *                        by the caller (Runtime::CloudTypeService) — a parameter rather than something
     *                        looked up here, because a pure function that reaches for a service is not a
     *                        pure function and this one is driven directly by three test suites. A layer
     *                        whose slots are all empty arrives as ONE Assets::CloudTypeDefaultShape, which
     *                        is the ONE place the "no type chosen" answer is decided; a count of zero is
     *                        answered here with an empty shell rather than with a guess, because a packer
     *                        that depends on its caller having checked something will one day be called by
     *                        someone who did not.
     * @param speciesCount    how many of @p shapes are filled, 0..kCloudSpeciesSlots. Above the ceiling it
     *                        is clamped rather than read past.
     * @param windOffsetWorld the drift accumulated by the ECS system, world units. It is a parameter
     *                        rather than a member because the accumulator belongs to the system that
     *                        owns the timestep, and a second copy here would be state that can disagree.
     * @param lightMarchSampleCeiling
     *                        the quality tier's ceiling on the shadow ray's sample count
     *                        (Graphic::CloudQualityScale::LightMarchSampleCeiling). It DEFAULTS TO THE
     *                        IDENTITY — ECS::kCloudLightMarchMaxSamples caps nothing the component's own
     *                        Range does not — so "no tier stated" means "the component's own number",
     *                        which is the only answer that is not a guess. It is not a fallback for a
     *                        failure: the renderer always states a tier, and the suites that drive this
     *                        packer directly are asserting the component's behaviour rather than a
     *                        machine's budget.
     * @param stopTransmittanceFloor
     *                        the tier's floor under the transmittance the march stops at
     *                        (Graphic::CloudQualityScale::StopTransmittanceFloor). Its identity is ZERO,
     *                        which floors nothing, for the reason given on that field.
     * @param noise           which of the four bound noise volumes each species reads, as
     *                        Graphic::ResolveCloudNoiseVolumes computed it from the layer's types. ITS
     *                        IDENTITY IS THE DEFAULT-CONSTRUCTED VALUE — every species on slot 0 — which
     *                        is what a layer whose types name one volume between them genuinely resolves
     *                        to, and what every layer in the repository resolved to before a type could
     *                        name one at all. It is an identity and not a fallback for a failure: the
     *                        renderer always states it, and the suites that drive this packer directly are
     *                        asserting the component's behaviour rather than the renderer's binding.
     *
     * THE TWO ATMOSPHERE COUPLINGS follow the sky model, gated on the same handle the height fog uses —
     * a non-null AtmosphereEnv::DistantSkyLight, published in SkyModel::PhysicalAtmosphere and nowhere
     * else. One gate rather than two, so the clouds cannot end up half physical:
     *
     *   * SUN. In the physical model the clouds are lit by SunIlluminanceOnGround — the directional
     *     light's own Color x Intensity after the atmosphere's transmittance, which is what actually
     *     falls on things. In the artistic gradient they are lit by SunIrradiance, the sky component's
     *     elevation-tinted disc brightness, because that is the quantity the gradient was authored
     *     against. The two differ by more than an order of magnitude in a typical scene and neither is a
     *     stand-in for the other.
     *
     *   * AMBIENT. In the physical model the value is a GPU texel and cannot be folded in here, so what
     *     travels is the artist's SCALE and a gate, and the shader does the multiply. In the artistic
     *     gradient the value is ZenithRadiance — the whole upper dome, which is what a cloud sees — and
     *     it is folded in here, with the gate at 0.
     *
     * WITHOUT AN ATMOSPHERE there is no sun and no sky, so both terms are zero rather than guessed. The
     * renderer does not dispatch at all in that case; the packer is written to be correct anyway, because
     * a packer that depends on its caller having checked something is a packer that will one day be
     * called by someone who did not.
     */
    /// Where the modelling volume the march will read has been baked, and how big it is. Handed to the
    /// packer rather than recomputed inside it, because the answer is the SERVICE's — the volume on the
    /// device was baked for one particular snapped origin, and a packer that derived its own from the
    /// camera would address a region that had not been baked yet on the frame the camera crossed a snap.
    /// That is the two-statements-of-one-fact defect class, and here it would show as the whole sky
    /// jumping by a lattice cell for one frame.
    struct CloudRegionBinding
    {
        glm::vec2 OriginKm{ 0.0f }; ///< the region's minimum corner, world kilometres
        float     SideKm = 1.0f;    ///< its horizontal side, and the period the volume tiles with
    };

    /**
     * @brief Which noise volumes a layer actually needs, and which of them each species reads.
     *
     * ONE STATEMENT OF A MAPPING THAT TWO PLACES CONSUME. The renderer binds the images and the packer
     * numbers them, and if those two disagreed by one the erosion of every cloud in the sky would be cut
     * from the wrong volume with nothing in the log — which is the two-statements-of-one-fact class this
     * subsystem has already paid for four times over (DEV_CONTRACT.md section 2.3.1). So the mapping is
     * computed here, once, and both sides read the same struct.
     *
     * PURE, and driven directly by Desert/Tests/Engine/CloudPayload: in, the volume handle of each
     * species; out, the deduplicated list and the index each species takes. No service, no device.
     */
    struct CloudNoiseResolution
    {
        /// How many DISTINCT volumes the layer needs, 1..kCloudSpeciesSlots. Never zero: a layer with no
        /// species still binds one volume, because a declared sampler with no image is an invalid
        /// descriptor set and this backend answers one by skipping the dispatch.
        uint32_t DistinctCount = 1;

        /// For species k, which of the DistinctCount volumes its edge is cut from. Slots at or past the
        /// species count are 0 — they are never read, and 0 is the one index that is always bound.
        uint32_t SlotOfSpecies[kCloudSpeciesSlots] = { 0u, 0u, 0u, 0u };

        /// The volume each of the DistinctCount slots holds, in slot order. Entries at or past
        /// DistinctCount repeat Volume[0], so every one of the four descriptors has a valid image
        /// whatever the layer names — see the note on kCloudNoiseBindings.
        Assets::AssetHandle Volume[kCloudSpeciesSlots] = {};
    };

    /**
     * @brief Deduplicate the layer's per-species noise volumes into the four slots the march binds.
     *
     * @param perSpecies   the volume handle of species 0..speciesCount-1. A null handle means "the
     *                     built-in default volume", which is what eight of the nine shipped types name —
     *                     and null is a VALUE here rather than a missing one, so two species that both
     *                     name nothing share a slot exactly as two that name the same file do.
     * @param speciesCount how many of @p perSpecies are filled, 0..kCloudSpeciesSlots. Above the ceiling
     *                     it is clamped rather than read past; zero gives one slot holding the null
     *                     handle, which the noise service resolves to the default.
     */
    inline CloudNoiseResolution ResolveCloudNoiseVolumes( const Assets::AssetHandle* perSpecies,
                                                          uint32_t                   speciesCount )
    {
        CloudNoiseResolution resolved;

        const uint32_t species = perSpecies ? std::min( speciesCount, kCloudSpeciesSlots ) : 0u;

        // Slot 0 always exists and always holds species 0's volume — or the null handle when the layer has
        // no species at all, which the service turns into the default. That is what makes DistinctCount a
        // count of one rather than of zero in the degenerate case, and it is why the loop below starts at
        // the second species rather than at the first.
        resolved.Volume[0] = species > 0u ? perSpecies[0] : Assets::AssetHandle::Null();

        for ( uint32_t k = 1; k < species; ++k )
        {
            uint32_t slot = resolved.DistinctCount;
            for ( uint32_t taken = 0; taken < resolved.DistinctCount; ++taken )
            {
                if ( resolved.Volume[taken] == perSpecies[k] )
                {
                    slot = taken;
                    break;
                }
            }

            if ( slot == resolved.DistinctCount )
            {
                resolved.Volume[resolved.DistinctCount] = perSpecies[k];
                ++resolved.DistinctCount;
            }

            resolved.SlotOfSpecies[k] = slot;
        }

        // Every unused descriptor repeats slot 0's image rather than being left unbound. An unbound
        // sampler is an INVALID descriptor set, not an unused one, and this backend answers an invalid set
        // by skipping the whole dispatch — the clouds would vanish with nothing in the log, which is a
        // failure this subsystem has shipped before.
        for ( uint32_t slot = resolved.DistinctCount; slot < kCloudSpeciesSlots; ++slot )
            resolved.Volume[slot] = resolved.Volume[0];

        return resolved;
    }

    /**
     * @brief Does this frame light the shell sample by sample, or with one colour?
     *
     * THE ONE PLACE THE QUESTION IS ANSWERED, and it has to be one place because three decisions hang off
     * it in three different files: PackCloudParams below chooses WHICH illuminance the parameter block
     * carries, VolumetricCloudRenderer chooses whether to raise CloudPush::Frame.y and bind the real LUT,
     * and the same renderer tells the environment bake through CloudEnvironmentBake. Those are the classic
     * pair this project keeps finding — individually correct, silently catastrophic when they disagree,
     * because the block would then carry the outer-space illuminance (an order of magnitude brighter at a
     * low sun) while a march applied no transmittance to it at all.
     *
     * ALL FOUR CONDITIONS ARE LOAD-BEARING:
     *   * the artist asked for it;
     *   * there is an evaluated atmosphere at all;
     *   * the transmittance LUT has actually been marched this frame — a null handle means the pair has
     *     not been baked, and the fallback texture bound in its place is not an atmosphere;
     *   * the DISTANT SKY LIGHT exists, which is this packer's existing definition of "the physical model
     *     is running". Without it the block carries SunIrradiance — the SKY's elevation-tinted disc
     *     brightness, never multiplied by any transmittance — and swapping THAT for an outer-space
     *     illuminance would not be a refinement, it would be a different quantity.
     */
    inline bool CloudUsesPerSampleSunTransmittance( const ECS::VolumetricCloudData& data,
                                                    const AtmosphereEnv&            atmosphere )
    {
        return data.PerSampleAtmosphereTransmittance && atmosphere.Valid &&
               atmosphere.TransmittanceLut != nullptr && atmosphere.DistantSkyLight != nullptr;
    }

    /**
     * @brief HOW HIGH THIS SCENE HANGS ITS DECK — applied to the TYPES, before anything derives anything.
     *
     * A cloud type states the altitudes of its own band, and it states them for every scene that loads
     * the file. A scene that wants its deck higher therefore cannot say so by editing the type, and until
     * this function existed it could not say so at all: the shell was the union of the types' bands and
     * nothing else could reach it.
     *
     * WHY THE SHAPES AND NOT THE SHELL, which is the whole of the decision. Lifting only
     * `CloudProceduralFieldParams::LayerBottomKm` would move the shell and leave the bodies behind: the
     * generator places each lump at `shape.BaseAltitudeKm + ...` in absolute kilometres and DISCARDS any
     * whose extent falls outside `[LayerBottomKm, LayerBottomKm + LayerThicknessKm]`
     * (Engine/Assets/CloudProceduralVolume.cpp, the per-blob rejection). The shell would rise and the sky
     * would empty. Lifting the SHAPE instead moves the band, the envelope computed from it, the packed
     * `CloudGpuPayload::Layer`, the baked bodies and the rebake key together — because every one of them
     * is downstream of this array. There is no second number to keep in agreement, which is the only kind
     * of agreement this subsystem has managed to keep.
     *
     * ONE-DIRECTIONAL, and clamped rather than refused. A `.desce` is a text file: an out-of-range number
     * in one must produce a sky rather than a refusal, exactly as the four placement numbers in
     * ApplyCloudMaterialToBakeParams argue. The clamp is the component's own Range and not a second
     * opinion about it.
     *
     * @param shapes  lifted in place; @p count of them, clamped to kCloudSpeciesSlots
     * @return the lift actually applied, in kilometres — what the caller may log and a test may assert
     */

    /// WHAT THE FILE ASKED FOR, MADE LEGAL, IN ONE PLACE. Two readers need this number and only one of
    /// them lifts anything: the renderer also prints it beside the baked envelope, because an envelope
    /// that moved says nothing about WHICH of its two causes moved it. Two copies of the clamp would be
    /// the §2.3.1 pair — individually correct, and silently disagreeing the day the Range changes.
    inline float CloudLayerLiftKm( const ECS::VolumetricCloudData& data )
    {
        // A CLAMP IS NOT ENOUGH, AND THE TEST FOUND IT. `std::clamp(NaN, lo, hi)` returns the NaN — it is
        // `v < lo ? lo : hi < v ? hi : v` and every comparison against a NaN is false — so a `.desce`
        // carrying `nan` would have moved every band to NaN, and a NaN band is a shell the validator
        // refuses: the whole layer would stop baking, with the error naming LayerBottomKm rather than the
        // knob that produced it. An unreadable offset means NO offset, which is the same answer a missing
        // key gives and the only one that still draws the sky the scene was authored with.
        if ( !std::isfinite( data.LayerAltitudeOffset ) )
            return 0.0f;

        return std::clamp( data.LayerAltitudeOffset, 0.0f, ECS::kCloudLayerAltitudeOffsetMaxKm );
    }

    inline float CloudLiftSpeciesSet( const ECS::VolumetricCloudData& data, CloudTypeShape* shapes,
                                      uint32_t count )
    {
        const float liftKm = CloudLayerLiftKm( data );
        if ( liftKm == 0.0f )
            return 0.0f;

        const uint32_t used = std::min( count, kCloudSpeciesSlots );
        for ( uint32_t i = 0; i < used; ++i )
        {
            // EVERY ALTITUDE IN THE SHAPE MOVES, the anvil's included. Moving the tower without the
            // canopy would make the lift a shape edit rather than a translation, and CloudTypeTopKm reads
            // the canopy's ceiling — the shell would then grow by the lift instead of rising by it. The
            // anvil of a type that has none is inert either way: Graphic::CloudTypeHasAnvil is the single
            // predicate that decides whether it is drawn or measured at all, and it reads the strength
            // and the thickness, neither of which is touched here.
            shapes[i].BaseAltitudeKm += liftKm;
            shapes[i].TopAltitudeKm += liftKm;
            shapes[i].AnvilAltitudeKm += liftKm;
        }

        return liftKm;
    }

    inline CloudGpuPayload PackCloudParams( const ECS::VolumetricCloudData& data,
                                            const CloudMaterialValues& material, const CloudTypeShape* shapes,
                                            uint32_t speciesCount, const AtmosphereEnv& atmosphere,
                                            const glm::vec3&          windOffsetWorld,
                                            const CloudRegionBinding& region  = CloudRegionBinding{},
                                            int32_t lightMarchSampleCeiling   = ECS::kCloudLightMarchMaxSamples,
                                            float   stopTransmittanceFloor    = 0.0f,
                                            const CloudNoiseResolution& noise = CloudNoiseResolution{} )
    {
        const bool     physical = atmosphere.Valid && atmosphere.DistantSkyLight != nullptr;
        const uint32_t species  = std::min( speciesCount, kCloudSpeciesSlots );

        // THE ENVELOPE IS COMPUTED, NOT AUTHORED, and this is the one place it is computed. It is the
        // UNION of the altitude ranges of the cloud types this layer actually carries — and that sentence
        // was written by T0 for a set of one and is unchanged for a set of four, which is the whole of
        // what T1 promised about extending this code and the first thing T3 checked.
        //
        // The reason is the class of defect §2.3.1 of the contract names: an authored shell and a type's
        // own altitudes are two numbers obliged to agree, each of them individually legal, and the symptom
        // of their disagreeing is not an error but a cumulonimbus with its anvil sliced off by a ceiling
        // nobody remembers setting. Computing it makes the agreement structural, and
        // Desert/Tests/Engine/ComponentReflection asserts `envelope ⊇ every active type` on the packed
        // block rather than on the intention — for every member of the set, which is the version of that
        // test a partition-based blend would never have needed and a union does.
        const CloudEnvelopeKm envelope = CloudTypeSetEnvelopeKm( shapes, species );

        const float bottomKm = species > 0 ? std::max( envelope.BottomKm, 0.0f ) : 0.0f;
        // Floored at a metre for the same reason CloudMakeLayer floors it: a shell of zero thickness has
        // a coincident pair of roots and every grazing ray produces a segment the step schedule divides
        // by. The library cannot produce one, but the packer does not depend on its caller having
        // checked something.
        const float thicknessKm = species > 0 ? std::max( envelope.TopKm - bottomKm, 0.001f ) : 0.001f;
        const float planetKm    = std::max( data.PlanetRadius, 1.0f );

        glm::vec3 sunDirection{ 0.0f, 1.0f, 0.0f };
        glm::vec3 sunIrradiance{ 0.0f };
        glm::vec3 ambient{ 0.0f };

        if ( atmosphere.Valid )
        {
            sunDirection = atmosphere.SunDirection;
            // THE SUN THE SHELL IS LIT BY, and which of the three it is depends on how far the marches are
            // going to carry the atmosphere themselves.
            //
            //   * no physical model      -> the SKY's own elevation-tinted disc brightness;
            //   * physical, one colour   -> illuminance x T(ground), which is UE's default and applies the
            //                               atmosphere ONCE, at sea level, to a shell kilometres tall;
            //   * physical, per sample   -> the OUTER-SPACE illuminance, with T moved into the march so
            //                               every sample gets the transmittance at its OWN altitude. The
            //                               absolute form and not a ratio: the base already has T(ground)
            //                               divided out of it by construction, because it never had it.
            sunIrradiance = CloudUsesPerSampleSunTransmittance( data, atmosphere )
                                 ? atmosphere.SunOuterSpaceIlluminance
                                 : ( physical ? atmosphere.SunIlluminanceOnGround : atmosphere.SunIrradiance );
            ambient       = physical ? material.AmbientScale : material.AmbientScale * atmosphere.ZenithRadiance;
        }

        CloudGpuPayload p{};
        p.Layer   = glm::vec4( planetKm, bottomKm, thicknessKm,
                               std::max( data.MaxViewDistance, 0.0f ) / kCloudWorldUnitsPerKm );
        // THE TIER RAISES THE STOP THRESHOLD RATHER THAN REPLACING IT — max(), the mirror image of the
        // min() the shadow-ray ceiling gets below, so the tier can only ever end the march EARLIER than
        // the artist asked and never later. Both compositions have to point the same way (cheaper) or a
        // tier stops being a budget and becomes a second opinion about the sky.
        const float stopTransmittance =
             std::clamp( std::max( data.StopTransmittance, stopTransmittanceFloor ), 0.0f, 1.0f );

        p.March = glm::vec4( static_cast<float>( std::clamp( data.MaxSteps, 8, 512 ) ), stopTransmittance,
                             std::max( data.TracingStartDistance, 0.0f ) / kCloudWorldUnitsPerKm,
                             std::max( material.ExtinctionScale, 0.0f ) );
        // Floored so the reciprocal is finite for a caller that has not bound a region yet — the frames
        // before the first bake finishes, where the species count is what stops the volume being read.
        const float regionSideKm = std::max( region.SideKm, 1e-3f );

        p.Region = glm::vec4( region.OriginKm.x, region.OriginKm.y, 1.0f / regionSideKm,
                              std::max( material.DetailTileSize, 1.0f ) / kCloudWorldUnitsPerKm );
        // .z IS ZERO AND UNREAD — it held the scalar albedo. Written zero rather than left indeterminate:
        // the payload is memcpy'd to the GPU, and an uninitialised byte in a buffer that is otherwise
        // deterministic would make two identical frames differ, which is the noise floor this subsystem's
        // whole verification method rests on. kCloudUnreadSlots carries its row.
        p.Detail = glm::vec4( std::clamp( material.DetailStrength, 0.0f, 1.0f ),
                              std::max( material.DensityScale, 0.0f ), 0.0f, static_cast<float>( species ) );

        // THE MEDIUM'S ALBEDO, PER COLOUR, clamped per channel. Clamped rather than trusted for the reason
        // every other clamp here gives — a scene file is a text file — and per CHANNEL because an albedo
        // above one is a medium that emits, which is what Emissive would be for if it existed: a channel
        // over 1 here would make the multiple-scattering series diverge instead of converging, and the
        // frame would bloom on the third octave with nothing in the log.
        p.Albedo =
             glm::vec4( glm::clamp( material.ScatteringAlbedo, glm::vec3( 0.0f ), glm::vec3( 1.0f ) ), 0.0f );

        // THE TYPES' FACTORS ARE NO LONGER FOLDED INTO THE LAYER'S, and the reason is arithmetic rather
        // than taste. A cumulonimbus is made of more water than a stratus, a cirrus is a quarter as opaque
        // as either, and a lenticular has almost no erosion at all — those are facts about the KIND of
        // cloud, and while a layer had exactly one kind the product `layer x type` could be formed here,
        // once, and sent as one number. Four kinds in one shell have four different products and the march
        // does not know which one it needs until it knows which species won the sample, so the factor
        // travels per species and the multiply moves to the point of use. The artist's Density Scale,
        // Extinction Scale and Detail Strength are still multipliers ON them, so "1" keeps meaning "these
        // types as they are"; what changed is where the multiplication happens, not what it means.
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            if ( slot >= species )
            {
                // Zero rather than the default type's numbers. An unfilled slot must not be able to put
                // cloud in the sky if the count is ever wrong, and a zero density factor is the state in
                // which it cannot.
                p.SpeciesEdge[slot] = glm::vec4( 0.0f );
                continue;
            }

            const CloudTypeShape& shape = shapes[slot];

            p.SpeciesEdge[slot] =
                 glm::vec4( std::clamp( shape.DetailCharacter, 0.0f, 1.0f ), std::max( shape.DetailFactor, 0.0f ),
                            std::max( shape.DensityFactor, 0.0f ), std::max( shape.ExtinctionFactor, 0.0f ) );
        }

        // THE INDEX IS CLAMPED TO A BOUND DESCRIPTOR, and not because the resolver can produce a bad one —
        // it cannot, by construction. It is clamped because this is the last point at which a number that
        // reaches a fetch selector can be checked at all: past here it is four floats in a buffer, and a
        // slot of 4 in the march is a branch chain that falls through to slot 0 silently. One clamp turns
        // "somebody widened kCloudSpeciesSlots on one side only" into a sky cut from the wrong volume in
        // ONE place rather than into a shader that reads a descriptor nobody wrote.
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            const uint32_t index = std::min( noise.SlotOfSpecies[slot], kCloudSpeciesSlots - 1u );
            p.SpeciesNoise[static_cast<int>( slot )] = static_cast<float>( index );
        }

        p.Wind = glm::vec4( windOffsetWorld / kCloudWorldUnitsPerKm, std::clamp( material.PhaseG, -0.9f, 0.9f ) );
        p.Sun     = glm::vec4( sunDirection, std::max( data.LightMarchDistance, 0.0f ) / kCloudWorldUnitsPerKm );
        // The ceiling is ECS::kCloudLightMarchMaxSamples and NOT a literal, because this clamp, the
        // slider's Range and the clamp inside CloudRaymarch.shader are three copies of one number. While
        // all three were literals they could disagree, and the disagreement is silent: the slider offers
        // a value, this line accepts it, and the shader throws it away.
        //
        // AND THE QUALITY TIER LOWERS THAT CEILING RATHER THAN REPLACING THE VALUE. min(), so the artist's
        // number is what a machine that can afford it renders and the tier can only ever make the frame
        // cheaper — a scene authored at 16 gets 16 on every tier, because asking for the cheap answer is
        // an authored intention and not a budget the tier is entitled to overrule. That is also what keeps
        // this from being a second source of truth for a field the component owns (contract §2.1): there
        // is one authored value and one ceiling, and the GPU gets their minimum.
        const int32_t lightSampleCeiling =
             std::clamp( lightMarchSampleCeiling, 1, ECS::kCloudLightMarchMaxSamples );
        p.SunColour = glm::vec4(
             sunIrradiance, static_cast<float>( std::clamp( data.LightMarchSamples, 1, lightSampleCeiling ) ) );
        p.Ambient      = glm::vec4( ambient, physical ? 1.0f : 0.0f );
        // The ceiling is ECS::kCloudMultiScatterMaxOctaves and NOT a literal, for the reason the shadow
        // ray's neighbour above gives: this clamp, the slider's Range and the clamp inside
        // Common/CloudLighting.glslh are three copies of one number.
        p.MultiScatter = glm::vec4( static_cast<float>( std::clamp( material.MultiScatterOctaves, 1,
                                                                    ECS::kCloudMultiScatterMaxOctaves ) ),
                                    std::clamp( material.MultiScatterContribution, 0.0f, 1.0f ),
                                    std::clamp( material.MultiScatterOcclusion, 0.0f, 1.0f ),
                                    std::clamp( material.MultiScatterEccentricity, 0.0f, 1.0f ) );

        // The aerial perspective is a coupling, not a dependency: without a volume the gate is 0 and the
        // shader composes the exact identity. The two scalars are read from AtmosphereEnv rather than
        // from the cloud component because they describe the VOLUME, and the volume belongs to the sky —
        // a second authored copy here is how the fill and the read end up disagreeing about the slice
        // mapping.
        p.Aerial = glm::vec3( atmosphere.AerialPerspectiveDepthKm, atmosphere.AerialPerspectiveViewDistanceScale,
                              atmosphere.AerialPerspectiveVolume != nullptr ? 1.0f : 0.0f );

        // The second phase lobe, the ambient-occlusion amount and the distance past which the layer is not
        // traced at all. The last is Unreal's TracingStartMaxDistance, and its absence was found on review:
        // without it a ray that enters the shell twelve thousand kilometres away — which the geometry can
        // legitimately report before the planet test rejects it — is a ray the march would still try.
        p.Phase = glm::vec4( std::clamp( material.PhaseGBackward, -0.9f, 0.9f ),
                             std::clamp( material.PhaseBlend, 0.0f, 1.0f ),
                             std::clamp( material.AmbientOcclusionStrength, 0.0f, 1.0f ),
                             std::max( data.TracingStartMaxDistance, 0.0f ) / kCloudWorldUnitsPerKm );
        // The two fades, both of which UE carries and neither of which is physics. The aerial perspective
        // is CORRECT at ninety kilometres and it correctly erases a cloud on the horizon; whether a sky is
        // wanted to look that way is an art decision, so it gets a dial rather than an argument. Zero fade
        // distance means "apply it in full from the camera", which is UE's default and the physical answer.
        //
        // The near fade is repaired as a PAIR — see CloudResolveNearFade — because its two fields feed a
        // smoothstep and GLSL leaves that undefined unless the second is strictly past the first.
        const CloudNearFadeKm nearFade =
             CloudResolveNearFade( data.NearFadeStartDistance, data.NearFadeEndDistance );

        p.Fade = glm::vec4( std::max( data.AerialPerspectiveStartDistance, 0.0f ) / kCloudWorldUnitsPerKm,
                            std::max( data.AerialPerspectiveFadeDistance, 0.0f ) / kCloudWorldUnitsPerKm,
                            nearFade.EndKm, nearFade.StartKm );
        return p;
    }

    // THERE IS NO BAKE KEY HERE ANY MORE. `CloudNoiseBakeKey` packed the component's two seeds and two
    // octave counts into a push constant for the compute bake that filled the noise volume; the volume is
    // now an asset generated offline (Engine/Assets/CloudNoiseVolume.hpp), so the pass, the push constant
    // and the four fields behind them are gone together rather than left as a struct nobody constructs.
} // namespace Desert::Graphic

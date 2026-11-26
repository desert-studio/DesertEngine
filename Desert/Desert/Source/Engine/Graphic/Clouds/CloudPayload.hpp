#pragma once

#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudLayout.hpp>
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
    inline constexpr uint32_t kCloudPayloadBytes = sizeof( CloudGpuPayload );

    /// How many floats travel in the block. DERIVED from the struct, so it cannot be a number somebody
    /// updates separately from the layout.
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
        /// The member's GLSL name, exactly as the block declares it (`CloudDetail`).
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
    inline constexpr std::array<CloudUnreadSlot, 3> kCloudUnreadSlots = {
         { { "CloudLayoutStrength", 'z',
             "the layout has two strengths and a grid of vec4s grows by four; the place took its own vec4 "
             "whole, so the two strengths leave two floats nobody has a number for" },
           { "CloudLayoutStrength", 'w', "as z" },
           { "CloudAerial", 'w',
             "the aerial perspective has three numbers and closes the block; a generated block is a whole "
             "number of its 16-byte alignment on both sides, so the last vec4 carries one float nobody has a "
             "number for" } } };

    /// How many of the block's floats a shader is expected to fetch. DERIVED, so the two halves of the
    /// claim — the layout and the exceptions — cannot be adjusted independently.
    inline constexpr uint32_t kCloudPayloadReadFloats =
         kCloudPayloadFloats - static_cast<uint32_t>( kCloudUnreadSlots.size() );

    // The march's per-dispatch push (camera, trace grid, frame gates, visualize) is ShaderPush::CloudRaymarch, CloudRaymarch.shader's PushConstants block.


    /**
     * Unreal's VolumetricRenderTarget sub-pixel walk, mode 0.
     *
     * Four TRACES cover the four half-resolution pixels of one quarter-resolution texel, in the order
     * {0, 2, 3, 1} rather than {0, 1, 2, 3}. Each sub-pixel is held for kCloudTraceGridPhases frames, one
     * per phase of the trace grid's 2x2 lattice (below), so the walk of one texel spans kCloudTraceWalkFrames
     * frames.
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

    /**
     * THE 2x2 LATTICE OVER THE TRACE GRID (CLOUD-TURN-CACHE-1): each frame traces the quarter-res texels of
     * ONE of four lattice phases, (x & 1, y & 1) == CloudTraceGridPhase(frame), and the temporal resolve
     * reconstructs the other three quarters from the history and from the traced quarter's lattice neighbours
     * (CloudTemporalResolve.shader TraceFiltered). Unreal's r.VolumetricRenderTarget walks are the same family
     * of one trace per pixel every N frames: mode 0 is N = 4 (the 2x2 sub-pixel walk above), mode 2 is N = 16
     * (its 4x4 Bayer table, VolumetricRenderTarget.cpp:314), and this walk is N = 16 over the half-resolution
     * reconstruction: a 4x4 block of half-res pixels is traced at (2 * phase + sub-pixel) on each axis, the
     * phase the 4x4 table's coarse level and the sub-pixel its fine one.
     *
     * THE PHASE ORDER IS THAT TABLE'S COARSE LEVEL, (0,0) -> (1,1) -> (1,0) -> (0,1): its frames 0..3 sit at
     * (0,0), (2,2), (2,0), (0,2) of the 4x4 block. Two consecutive frames from an even one are diagonally
     * opposite and together one checkerboard, so no region of the screen goes two frames without a sample
     * within a texel of it, and four frames cover the whole quarter grid once. The phase advances EVERY frame
     * and the sub-pixel every fourth one, so a texel of any phase visits all four sub-pixels in
     * kCloudTraceWalkFrames frames. The march dispatches only this phase's texels (a quarter of the threads,
     * no idle lanes inside the grid); the untraced three quarters of the transient trace are never read.
     *
     * (The two-phase checkerboard this replaces, CLOUD-MARCH-COST-3, was N = 8: it halved the rays, this halves
     * them again, and the per-trace history weights stay what they were measured as — CloudTemporalResolve's
     * kStillOwnedSampleWeight says what that costs in frames.)
     */
    struct CloudTracePhase
    {
        uint32_t X;
        uint32_t Y;

        // The one encoding both shaders decode (ShaderPush::CloudRaymarch::CloudVisualize.y, CloudResolveParams::TraceGridPhase):
        // bit 0 the column's parity, bit 1 the row's.
        [[nodiscard]] constexpr uint32_t Index() const
        {
            return X | ( Y << 1u );
        }
    };

    inline constexpr uint32_t kCloudTraceGridPhases = 4;

    // Frames between two traces of one half-resolution pixel: four sub-pixels times four lattice phases. The
    // march's start offset advances once per trace of a pixel, i.e. once per this many frames.
    inline constexpr uint32_t kCloudTraceWalkFrames = kCloudTraceSubPixelCount * kCloudTraceGridPhases;

    inline constexpr CloudTracePhase CloudTraceGridPhase( uint32_t frameIndex )
    {
        // Indexed by CloudTracePhase::Index: 0 = (0,0), 3 = (1,1), 1 = (1,0), 2 = (0,1).
        constexpr uint32_t order[kCloudTraceGridPhases] = { 0u, 3u, 1u, 2u };

        const uint32_t index = order[frameIndex % kCloudTraceGridPhases];
        return CloudTracePhase{ index & 1u, index >> 1u };
    }

    inline constexpr CloudSubPixel CloudTraceSubPixel( uint32_t frameIndex )
    {
        constexpr uint32_t order[kCloudTraceSubPixelCount] = { 0u, 2u, 3u, 1u };

        const uint32_t index = order[( frameIndex / kCloudTraceGridPhases ) % kCloudTraceSubPixelCount];
        return CloudSubPixel{ index % 2u, index / 2u };
    }

    // Every half-resolution pixel of a 4x4 block is traced exactly once per walk: the walk is a permutation of
    // the block, as Unreal's 4x4 table is.
    inline constexpr bool CloudTraceWalkCoversBlock()
    {
        uint32_t visited = 0u;
        for ( uint32_t frame = 0u; frame < kCloudTraceWalkFrames; ++frame )
        {
            const CloudTracePhase phase = CloudTraceGridPhase( frame );
            const CloudSubPixel   sub   = CloudTraceSubPixel( frame );
            visited |= 1u << ( ( 2u * phase.Y + sub.Y ) * 4u + 2u * phase.X + sub.X );
        }
        return visited == 0xFFFFu;
    }

    static_assert( CloudTraceSubPixel( 0u ).X == 0u && CloudTraceSubPixel( 0u ).Y == 0u );
    static_assert( CloudTraceSubPixel( 4u ).X == 0u && CloudTraceSubPixel( 4u ).Y == 1u );
    static_assert( CloudTraceSubPixel( 8u ).X == 1u && CloudTraceSubPixel( 8u ).Y == 1u );
    static_assert( CloudTraceSubPixel( 12u ).X == 1u && CloudTraceSubPixel( 12u ).Y == 0u );
    // The four phases of one sub-pixel are four consecutive frames.
    static_assert( CloudTraceSubPixel( 3u ).X == CloudTraceSubPixel( 0u ).X &&
                   CloudTraceSubPixel( 15u ).Y == CloudTraceSubPixel( 12u ).Y );
    static_assert( CloudTraceGridPhase( 0u ).Index() == 0u && CloudTraceGridPhase( 1u ).Index() == 3u &&
                   CloudTraceGridPhase( 2u ).Index() == 1u && CloudTraceGridPhase( 3u ).Index() == 2u );
    static_assert( CloudTraceWalkCoversBlock(), "The walk must trace every half-res pixel of a 4x4 block once." );
    static_assert( CloudTraceSubPixel( kCloudTraceWalkFrames ).X == CloudTraceSubPixel( 0u ).X &&
                        CloudTraceGridPhase( kCloudTraceWalkFrames ).Index() == CloudTraceGridPhase( 0u ).Index(),
                   "The walk must be periodic." );

    inline constexpr uint32_t kCloudResolveParamsBytes = sizeof( CloudResolveParams );

    // One world unit is one centimetre (Common::Units), so a kilometre is 100 000 of them. Mirrored by
    // CLOUD_WORLD_UNITS_PER_KM in Common/CloudGeometry.glslh.
    inline constexpr float kCloudWorldUnitsPerKm = 100000.0f;

    // ---------------------------------------------------------------------------------------------
    // THE MARCH'S SEARCH RESOLUTION, ON THE C++ SIDE
    // ---------------------------------------------------------------------------------------------
    //
    // Mirrored from CLOUD_DISTANCE_TO_MAX_STEPS_KM in Common/CloudGeometry.glslh (the suites that compile
    // that header as C++ — CloudType, CloudShadow — read the shader's own CloudFinestResolvableChordKm;
    // this one is what the generator reads). Mirroring rather than including is the arrangement
    // kCloudWorldUnitsPerKm above already records, and for the same reason: the engine does not put the
    // shader root on its include path.
    //
    // WHY THE ENGINE NEEDS IT AT ALL, which it did not before phase Э5. The procedural producer's lumps
    // are now placed on the CPU, and how small a lump may be is decided by what the march can FIND rather
    // than by what it can integrate: outside cloud the ray strides by the coarse step and only drops to
    // the fine tier once a coarse sample has already found material, so a body that fits between two
    // coarse samples is never seen and whether it fits is decided by the ray's jitter. The generator
    // clamps against this, so lowering Max Steps buys coarser clouds instead of speckle.
    //
    // ONE CONSTANT, NOT TWO, since CLOUD-MARCH-COST-1: on a short segment the search stride IS the fine
    // step, CLOUD_DISTANCE_TO_MAX_STEPS_KM over the ceiling, so the coarse multiplier no longer enters the
    // finest chord (it bounds the stride on long segments only, which no chord bound reads).
    inline constexpr float kCloudDistanceToMaxStepsKm = 16.0f;

    /// The finest chord the schedule can resolve anywhere, kilometres. @p maxCount is the component's
    /// Max Steps. Two search strides — Nyquist against the SEARCH lattice, whose finest stride is the
    /// short-segment fine step.
    inline float CloudFinestResolvableChordKm( float maxCount )
    {
        const float searchStepKm = kCloudDistanceToMaxStepsKm / std::max( maxCount, 1.0f );
        return 2.0f * searchStepKm;
    }

    // THE CONSERVATIVE PROBE'S RADIUS, kilometres (CLOUD-MARCH-COST-1). Mirrored from CLOUD_COLUMN_PROBE_RADIUS_KM
    // in Common/CloudGeometry.glslh, on the terms the constant above is: the march jumps clear air on the promise
    // that no column within this radius holds cloud, and CloudProceduralClipmap keeps that promise by dilating
    // each level's column maximum by this many kilometres (CloudColumnProbeDilationTexels). A shader radius
    // above this number would let the march jump over cloud; one below only jumps less.
    inline constexpr float kCloudColumnProbeRadiusKm = 0.1875f;

    /// How many texels the column-max image of a level whose voxel is @p voxelKm is dilated by, each way: the
    /// probe radius in that level's texels, plus the one texel a trilinear fetch reaches past the texel its
    /// address falls in (the probe reads the texel, the march the 2 x 2 footprint around the address).
    inline uint32_t CloudColumnProbeDilationTexels( float voxelKm )
    {
        const float texels = std::ceil( kCloudColumnProbeRadiusKm / std::max( voxelKm, 1e-6f ) );
        return static_cast<uint32_t>( std::min( texels, 1.0e4f ) ) + 1u;
    }

    // THE CONSERVATIVE DENSITY'S HEIGHT BANDS (CLOUD-MARCH-COST-1 pass 5). Mirrored by CLOUD_COLUMN_MAX_BANDS in
    // Common/CloudField.glslh. Unreal's ConservativeDensity is evaluated at the sample's own height, so its zero
    // is tight vertically; a column maximum over the layer's whole height is zero only where the column holds no
    // cloud at ANY height, which on a deck of separate bodies is almost nowhere once dilated. The column-max image
    // therefore keeps each level's 32 rows as this many bands of equal height (a band = the maximum over its own
    // rows, per species), and the probe reads only the bands the skipped stretch of the ray can fetch from. Must
    // divide Assets::kCloudProceduralVolumeHeight (asserted where the bands are built,
    // CloudProceduralClipmap.cpp).
    inline constexpr uint32_t kCloudColumnMaxBands = 4u;

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
     * The shader's gate (`CloudFadeParams.z > 0`) is exactly equivalent to `end > start` under this
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
        /// Per clip level, finest first: xy the minimum corner (km), z 1 / side (1/km), w voxel (km) —
        /// CloudProceduralClipmap::LevelUniforms of the clipmap the march reads. The default is finite.
        std::array<glm::vec4, kCloudClipLevelSlots> Levels{ glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ),
                                                            glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ),
                                                            glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ) };
        /// The cut the march makes against the bake's rank — Assets::CloudFarWeatherUniform of the SAME
        /// parameters the bound volume was baked from, so the cover and the softness belong to the bytes
        /// they are compared with. Zero (the default) keeps nothing: no bake, no cut to make.
        glm::vec4 Weather{ 0.0f };
        /// Assets::CloudLayoutPlaceUniform / CloudLayoutStrengthUniform of the same parameters. Zero strength
        /// (the default) reads no painting.
        glm::vec4 LayoutPlace{ 0.0f };
        glm::vec2 LayoutStrength{ 0.0f };
        /// The far path's statistics of the coarsest level on the device (CloudProceduralClipmap::
        /// FarStatistics). Zero (the default) draws no cloud past that level's window.
        std::array<glm::vec4, kCloudFarBandSlots> FarPresence{};
        std::array<glm::vec4, kCloudFarBandSlots> FarProfile{};
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
        /// whatever the layer names — see the note at CloudNoise1Texture in Programs/Clouds/CloudRaymarch.shader.
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
     * carries, VolumetricCloudRenderer chooses whether to raise ShaderPush::CloudRaymarch::CloudFrame.y and bind the real LUT,
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

    /// THE WISPY BASE OF ONE SPECIES, in the layer's height fraction: x where the type's band starts, y where
    /// its altitude density H first stands at its maximum (CloudProfileWispTopFraction). The packer sends it
    /// as CloudGpuPayload::SpeciesWispBase/Top and the C++ mirrors of the march bind it through this same
    /// function. A curve that peaks at its first sample gives y == x: no wispy base, the type's character
    /// throughout.
    inline glm::vec2 CloudSpeciesWispSpan( const CloudTypeShape& shape, float layerBottomKm,
                                           float layerThicknessKm )
    {
        const float thicknessKm = std::max( layerThicknessKm, 1e-3f );
        const float bandKm      = std::max( shape.TopAltitudeKm - shape.BaseAltitudeKm, 0.0f );
        const float wispTopKm   = shape.BaseAltitudeKm + CloudProfileWispTopFraction( shape.Profile ) * bandKm;
        return glm::vec2( ( shape.BaseAltitudeKm - layerBottomKm ) / thicknessKm,
                          ( wispTopKm - layerBottomKm ) / thicknessKm );
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
        // The levels as the clipmap reports them; z floored so the reciprocal side stays finite whatever a
        // caller hands in.
        for ( uint32_t level = 0; level < kCloudClipLevelSlots; ++level )
            p.Level[level] = glm::vec4( region.Levels[level].x, region.Levels[level].y,
                                        std::max( region.Levels[level].z, 1e-6f ), region.Levels[level].w );
        p.Detail = glm::vec4(
             std::clamp( material.DetailStrength, 0.0f, 1.0f ), std::max( material.DensityScale, 0.0f ),
             std::max( material.DetailTileSize, 1.0f ) / kCloudWorldUnitsPerKm, static_cast<float>( species ) );

        // THE MEDIUM'S ALBEDO, PER COLOUR, clamped per channel. Clamped rather than trusted for the reason
        // every other clamp here gives — a scene file is a text file — and per CHANNEL because an albedo
        // above one is a medium that emits, which is what Emissive would be for if it existed: a channel
        // over 1 here would make the multiple-scattering series diverge instead of converging, and the
        // frame would bloom on the third octave with nothing in the log.
        p.Albedo = glm::vec4( glm::clamp( material.ScatteringAlbedo, glm::vec3( 0.0f ), glm::vec3( 1.0f ) ),
                              std::clamp( material.SkyLightCloudBottomOcclusion, 0.0f, 1.0f ) );

        // THE TYPES' FACTORS ARE NO LONGER FOLDED INTO THE LAYER'S, and the reason is arithmetic rather
        // than taste. A cumulonimbus is made of more water than a stratus, a cirrus is a quarter as opaque
        // as either, and a lenticular has almost no erosion at all — those are facts about the KIND of
        // cloud, and while a layer had exactly one kind the product `layer x type` could be formed here,
        // once, and sent as one number. Four kinds in one shell have four different products and the march
        // does not know which one it needs until it knows which species won the sample, so the factor
        // travels per species and the multiply moves to the point of use. The artist's Density Scale,
        // Extinction Scale and Detail Strength are still multipliers ON them, so "1" keeps meaning "these
        // types as they are"; what changed is where the multiplication happens, not what it means.
        Assets::AssetHandle authoredTypes[ECS::kCloudTypeSlots];
        material.TypeSlots( authoredTypes );
        float typeWeights[ECS::kCloudTypeSlots];
        material.TypeWeights( typeWeights );
        const ECS::CloudSpeciesResolution typeResolution = ECS::ResolveCloudSpecies( authoredTypes );
        for ( uint32_t slot = 0; slot < kCloudSpeciesSlots; ++slot )
        {
            const int at = static_cast<int>( slot );
            // The built-in default has no authored slot; it answers to the first weight, as the slot it stands in.
            p.SpeciesCover[at] =
                 slot < species
                      ? std::clamp(
                             typeWeights[typeResolution.BuiltInDefault ? 0u : typeResolution.AuthoredSlot[slot]],
                             0.0f, 1.0f )
                      : 0.0f;
            if ( slot >= species )
            {
                // Zero rather than the default type's numbers. An unfilled slot must not be able to put
                // cloud in the sky if the count is ever wrong, and a zero density factor is the state in
                // which it cannot.
                p.SpeciesEdge[slot]   = glm::vec4( 0.0f );
                p.SpeciesWispBase[at] = 0.0f;
                p.SpeciesWispTop[at]  = 0.0f;
                continue;
            }

            const CloudTypeShape& shape = shapes[slot];

            const glm::vec2 wisp  = CloudSpeciesWispSpan( shape, bottomKm, thicknessKm );
            p.SpeciesWispBase[at] = wisp.x;
            p.SpeciesWispTop[at]  = wisp.y;

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
        p.Weather        = region.Weather;
        // THE COVERAGE THRESHOLD IS THIS FRAME'S, not the bake's (WTH-CLOUDS): the region's uniform is a function
        // of the baked parameters, which no longer change with Coverage, so the live value comes from the
        // material resolved this frame — the .demat chain with the weather's DynamicParams laid over it.
        p.Weather.z      = std::clamp( material.Coverage, 0.0f, 1.0f );
        p.LayoutPlace    = region.LayoutPlace;
        p.LayoutStrength = glm::vec4( region.LayoutStrength, 0.0f, 0.0f );
        for ( uint32_t band = 0; band < kCloudFarBandSlots; ++band )
        {
            p.FarPresence[band] = region.FarPresence[band];
            p.FarProfile[band]  = region.FarProfile[band];
        }

        p.Aerial = glm::vec4( atmosphere.AerialPerspectiveDepthKm, atmosphere.AerialPerspectiveViewDistanceScale,
                              atmosphere.AerialPerspectiveVolume != nullptr ? 1.0f : 0.0f, 0.0f );

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

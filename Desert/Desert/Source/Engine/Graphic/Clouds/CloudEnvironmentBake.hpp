#pragma once

#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class Image2D;
    class Image3D;

    /**
     * THE SEAM BETWEEN THE CLOUDS AND THE SKY'S ENVIRONMENT BAKE.
     *
     * The panorama the IBL chain is built from is baked by SkyboxRenderer, and the cloud field belongs to
     * VolumetricCloudRenderer — its sibling under the same SceneRenderer, which owns the parameters, the
     * modelling volume and the noise volumes and shares none of them. There is no route from one to the
     * other that does not cross a seam, so the seam is declared here rather than discovered as a widening
     * argument list.
     *
     * TWO STRUCTS AND NOT ONE, because they describe different things: what the cloud renderer KNOWS
     * (CloudEnvironmentBake — bytes and shared images) and what the bake pass BINDS
     * (CloudBakeBinding — device buffers the sky renderer filled with those bytes). Collapsing them would
     * force the cloud renderer to own a buffer for a dispatch it does not issue.
     */

    // ---------------------------------------------------------------------------------------------------
    // The bake pass's cloud descriptors
    // ---------------------------------------------------------------------------------------------------

    // ComputePipeline::SetInput / SetStorageBuffer take the binding as an explicit argument and never
    // consult the shader's reflection, so every number here must equal the one written in
    // Programs/Compute/BakeProceduralSky.shader. A mismatch lands a resource on a different descriptor
    // rather than on an error.
    //
    // FOUR, NOT ONE, for the parameter block. Graphic::kSkyPayloadBinding is 1 and so is the cloud block's
    // default (Common/CloudParams.glslh's CLOUD_PARAMS_BINDING) — this is the one pass in the engine that
    // reads both, and two blocks on one descriptor is not a diagnosable failure but one of them reading
    // the other's bytes. The shader takes the override; this is the C++ half of the same number.
    inline constexpr uint32_t kSkyBakeCloudParamsBinding                     = 4;
    inline constexpr uint32_t kSkyBakeCloudNoiseBindings[kCloudSpeciesSlots] = { 5, 6, 7, 8 };
    inline constexpr uint32_t kSkyBakeCloudModellingBinding                  = 9;
    inline constexpr uint32_t kSkyBakeCloudAuthoredAtlasBinding              = 10;
    inline constexpr uint32_t kSkyBakeCloudAuthoredBinding                   = 11;
    inline constexpr uint32_t kSkyBakeDistantSkyLightBinding                 = 12;
    inline constexpr uint32_t kSkyBakeCloudSkyOcclusionBinding               = 13;
    // The world weather map — the march's remap of the baked profile (CloudGpuPayload::Weather), applied
    // to the panorama exactly as to the screen.
    inline constexpr uint32_t kSkyBakeCloudFarWeatherBinding = 14;
    // The painted layout's two textures (CloudLayoutPatternTexture / CloudLayoutMaskTexture,
    // VolumetricCloudRenderer::SampledVolumes).
    inline constexpr uint32_t kSkyBakeCloudLayoutPatternBinding = 15;
    inline constexpr uint32_t kSkyBakeCloudLayoutMaskBinding    = 16;
    // The bake's two shared sampler states (ComputePipeline::SetSampler), the states the screen passes read the
    // same images through: LinearRepeatSampler for the cloud volumes and maps (CloudRaymarch), and
    // GlobalFilterSampler for the sky's LUTs and the distant sky light (the image's own Global-policy state).
    // The authored medium's textures read through its generated CloudMediumSampler, bound right after them.
    inline constexpr uint32_t kSkyBakeLinearRepeatSamplerBinding = 17;
    inline constexpr uint32_t kSkyBakeGlobalFilterSamplerBinding = 18;

    // ---------------------------------------------------------------------------------------------------
    // What the cloud renderer hands over
    // ---------------------------------------------------------------------------------------------------

    /**
     * This view's cloud field, prepared for a bake that runs OUTSIDE the frame.
     *
     * Every image here is SHARED, not borrowed: the cloud renderer (or, for the noise volumes and the
     * hero-cloud atlas, a Runtime service) owns them, and the sky light's capture that marches them is
     * time-sliced over many frames (SkyCapture), during which the cloud renderer may reallocate a volume
     * (a modelling clipmap rebuilt at a new size, an occlusion volume recreated, a noise asset swapped).
     * The capture keeps these references until its last slice is recorded, so it always marches the field
     * it began with and never a freed image.
     */
    struct CloudEnvironmentBake
    {
        /// false when this view has no cloud layer to bake — no component, disabled, no atmosphere to
        /// light it, or a resource that could not be built. The panorama is then the sky alone, bit for
        /// bit what it was before clouds reached the bake at all.
        bool Marched = false;

        /// true when this view HAS a cloud layer whose inputs are still arriving: a noise volume or a
        /// painted layout on a worker, the first modelling bake in flight, or a sky-occlusion volume the
        /// frame has not yet had a chance to write. A panorama baked now would be baked again the moment
        /// they land (measured on Clouds_Protocol: 372 ms sky-only, then 477 ms, then 461 ms), so the sky
        /// waits for them instead. A resource that FAILED is not pending: that layer bakes without clouds.
        bool InputsPending = false;

        /// Whether SkyOcclusionVolume holds a reconstruction that may be read. It is the SCREEN march's
        /// own gate, carried so the baked dome self-occludes exactly as much as the visible one does — and
        /// it is folded into the fingerprint below, because the volume is only written from the frame the
        /// first bake precedes, so the first bake of a scene necessarily misses it and has to be redone.
        bool SkyOcclusionValid = false;

        /// Whether CloudGpuPayload::SunColour below carries the sun's OUTER-SPACE illuminance, so that the
        /// bake must apply the atmosphere's transmittance at each sample's own altitude itself.
        ///
        /// IT TRAVELS BECAUSE THE BLOCK IS SHARED AND ITS MEANING IS NOT SELF-DESCRIBING. Params is the
        /// same block the screen march reads, and Graphic::CloudUsesPerSampleSunTransmittance changes what
        /// SunColour MEANS rather than adding a field beside it. A bake that took the block and applied no
        /// transmittance would light the whole IBL panorama with the sun as seen from space — several times
        /// the ground-level colour at a low sun — and it would look like an exposure bug, not like a
        /// missing multiply.
        bool PerSampleSunTransmittance = false;

        CloudGpuPayload      Params{};
        CloudAuthoredPayload Authored{};

        std::shared_ptr<Image3D> Noise[kCloudSpeciesSlots];
        std::shared_ptr<Image3D> Modelling;
        std::shared_ptr<Image2D> FarWeather;    // the world weather map the remap reads
        std::shared_ptr<Image2D> LayoutPattern; // the painted layout the cut reads (WX-PAINT)
        std::shared_ptr<Image2D> LayoutMask;
        std::shared_ptr<Image3D> AuthoredAtlas; // null in every scene with no hero cloud
        std::shared_ptr<Image3D> SkyOcclusionVolume;

        /// The compile-time substitution the three IN-FRAME cloud programs were compiled under, carried
        /// so the panorama that LIGHTS the scene is compiled under the same one.
        ///
        /// IT CROSSES THIS SEAM BECAUSE THE FOURTH CONSUMER BELONGS TO ANOTHER RENDERER. The march, the
        /// shadow map and the occlusion volume are the cloud renderer's and it compiles them itself;
        /// BakeProceduralSky is the SKY's program — atmosphere and clouds in one panorama, needed by
        /// scenes with no cloud layer at all — so it can never be a pass of a cloud material. Handing the
        /// variant across is what stops the world's light coming from a sky judged by a different medium
        /// than the one on screen: the same defect shape as the grey clouds, arriving through the IBL.
        Core::ShaderVariant Medium;

        /// The authored medium's OWN parameter block, already packed — one vec4 per property of ITS
        /// schema, in that schema's order (Engine/Graphic/Clouds/CloudMediumValues.hpp). Empty for the
        /// shipped medium and for any medium that exposes nothing, and the bake then binds no such
        /// descriptor, because the medium it is compiled with declares none.
        ///
        /// COPIED, as the images below are shared: these bytes are the cloud renderer's own per-frame
        /// resolve and would be overwritten before the capture's slices ran.
        std::vector<glm::vec4> MediumValues;

        /// The authored medium's images, one per declared slot, null where the material assigned nothing.
        /// Shared like every image here; the bake substitutes a fallback for a null so that a declared
        /// sampler is never left unwritten.
        std::vector<std::shared_ptr<Image2D>> MediumImages;

        /// What the panorama on the device was baked from, on the cloud side. See
        /// CloudEnvironmentFingerprint.
        uint64_t Fingerprint = 0;
    };

    /**
     * The same field, as the descriptors the bake dispatch writes.
     *
     * Params and Authored are the SKY renderer's buffers, filled from CloudEnvironmentBake's bytes: the
     * cloud renderer's own parameter buffer is a per-frame resource written by the passes inside the
     * frame, and the bake is issued before them.
     *
     * HELD FOR THE WHOLE CAPTURE. SkyCapture keeps this pack from Begin until its last slice is recorded
     * (or the capture is abandoned): the buffers and images bound to the faces' pipeline route at Begin
     * are read by every face slice after it, over many frames, so they are co-owned here -- a volume the
     * cloud renderer reallocates mid-capture, or the beginning view's renderer (whose buffers Params,
     * Authored and MediumParams are) going away, cannot free what a later slice reads.
     */
    struct CloudBakeBinding
    {
        bool Marched      = false;
        bool SkyOcclusion = false;

        /// CloudEnvironmentBake::PerSampleSunTransmittance, carried through. The bake shader needs no
        /// descriptor of its own for it: it already binds the SKY's transmittance LUT
        /// (Graphic::kSkyTransmittanceLutBinding) and already derives the shell's two radii from the sky
        /// parameter block it reads, so only the gate crosses this seam.
        bool PerSampleSunTransmittance = false;

        /// The atmosphere's Aerial Perspective Start Depth, kilometres. It reaches the screen march baked
        /// into the aerial-perspective volume; the bake integrates the air itself and so has to be told.
        float AerialStartDepthKm = 0.0f;

        std::shared_ptr<ShaderResources::StorageBuffer> Params;
        std::shared_ptr<ShaderResources::StorageBuffer> Authored;

        std::shared_ptr<Image3D> Noise[kCloudSpeciesSlots];
        std::shared_ptr<Image3D> Modelling;
        std::shared_ptr<Image2D> FarWeather;
        std::shared_ptr<Image2D> LayoutPattern;
        std::shared_ptr<Image2D> LayoutMask;
        std::shared_ptr<Image3D> AuthoredAtlas;
        std::shared_ptr<Image3D> SkyOcclusionVolume;

        /// The authored medium the bake's own program has to be compiled under — see
        /// CloudEnvironmentBake::Medium. Borrowed for the Begin call only (the faces' program is acquired
        /// there and kept by the capture); SkyCapture clears it before it keeps the pack. Null and default
        /// both mean the shipped medium, which is the same thing and needs no branch of its own.
        const Core::ShaderVariant* Medium = nullptr;

        /// The medium's own parameter block, on the SKY renderer's buffer for the reason Params and
        /// Authored are on it: the cloud renderer's copies are per-frame resources written by the passes
        /// inside the frame, and the bake is issued before them. Null when the medium declares no values,
        /// which is when its program declares no such block either — the two are the same fact.
        std::shared_ptr<ShaderResources::StorageBuffer> MediumParams;

        /// One entry per image slot the medium declares, null where the material assigned nothing. The
        /// COUNT is what the bake binds: a declared sampler left unwritten invalidates the descriptor set.
        std::vector<std::shared_ptr<Image2D>> MediumImages;
    };

    // ---------------------------------------------------------------------------------------------------
    // When the panorama has to be baked again
    // ---------------------------------------------------------------------------------------------------

    /**
     * @brief Everything about the clouds that the baked environment can see, as one comparable number.
     *
     * A bake costs three quarters of a second and idles the device for all of it, so the panorama is
     * rebuilt on a trigger rather than per frame. The sun is one half of that trigger and already exists
     * (Graphic::ShouldRebakeSkyEnvironment); this is the other half.
     *
     * WHAT IS DELIBERATELY NOT IN IT, and this is the whole content of the function:
     *
     *   * THE WIND. The diffuse irradiance cube is a 65 536-sample cosine convolution per texel — it
     *     integrates the ARRANGEMENT of the field away and responds only to the dome's mean, so advection
     *     moves nothing it can see. Rebaking on wind would mean rebaking every frame of every scene with a
     *     breeze in it, for a result that is the same to within the convolution's own noise.
     *   * THE MODELLING REGION'S ORIGIN, for the same reason and one more: it follows the CAMERA, snapped
     *     to the lump lattice, so a fingerprint that included it would idle the device every time a player
     *     walked three kilometres.
     *   * THE FAR PATH'S STATISTICS (FarPresence / FarProfile), for the origin's reason: they are measured
     *     over the coarsest clip level's WINDOW, which follows the camera, and recomputed every time that
     *     level scrolls -- the same field over a shifted window, equal to the last bits of a float sum or
     *     to the lattice's sampling noise. In the key they started a whole time-sliced recapture of the sky
     *     light at every scroll (measured on Clouds_Protocol: the camera placed at the shot's start moved
     *     the window by 47 m, FarProfile changed in its last bit, and 54 frames of capture followed for a
     *     ground that moved 0.06 grey levels). What decides the field they describe -- the procedural
     *     parameters -- reaches the key as @p shapeGeneration.
     *
     * What IS in it is everything else the march reads — density, extinction, albedo, the phase, the
     * multiple-scattering series, the fades, the four species' materials, the layer's envelope and the
     * sun. Taking the WHOLE block minus the two exclusions is what makes this hard to get wrong later: a
     * parameter added to CloudGpuPayload is in the fingerprint the moment it exists, where an enumerated
     * list of fields would silently miss it.
     *
     * PER-SAMPLE SUN TRANSMITTANCE NEEDS NO INPUT OF ITS OWN HERE, and that is worth stating because the
     * sky-occlusion flag beside it does. Turning it on replaces SunColour in the block — the ground-level
     * product becomes the outer-space illuminance — so it is already inside the bytes below. The
     * sky-occlusion flag is not in them at all, which is why it is a parameter.
     *
     * AND WHAT THE BLOCK CANNOT SEE ARRIVES AS @p shapeGeneration. Coverage, the cloud types, the seed,
     * the placement lattice and the painted layout decide WHERE cloud is, and none of them is in the
     * packed block at all — they are consumed on the CPU by Assets::BakeCloudProceduralVolume and reach
     * the march only as the contents of the modelling volume. A fingerprint over the block alone would
     * therefore miss the most important knob on the panel. The counter is bumped by the renderer whenever
     * that volume is rebuilt from parameters that DIFFER from the previous ones under
     * Assets::CloudProceduralParamsEqual — which is the canonical comparison, and which excludes the
     * region's origin, so following the camera does not count as a change.
     *
     * @param marched           false when there is no cloud layer at all, which answers 0.
     * @param skyOcclusionValid whether the march that produced @p payload could read the sky-occlusion
     *                          volume. It changes the baked radiance, so it changes the fingerprint.
     * @param shapeGeneration   how many times this view's modelling volume has been rebuilt from
     *                          genuinely different parameters.
     * @param mediumVariant     Core::ShaderVariant::Hash() of the authored medium the march is compiled
     *                          under; 0 is the shipped one. IT IS IN THE FINGERPRINT BECAUSE IT IS IN
     *                          THE PICTURE and in nothing else here: the medium is CODE, so it changes
     *                          what the panorama shows while leaving every byte of the packed block
     *                          identical. Left out, authoring a medium would move the sky on screen and
     *                          leave the light in the world coming from the previous one, indefinitely —
     *                          the "middle link drops a property" shape, in the one link that costs
     *                          three quarters of a second to re-run.
     * @param mediumValues      Graphic::CloudMediumValuesFingerprint of that medium's OWN parameters and
     *                          images; 0 when it exposes none. A SEPARATE INPUT AND NOT MIXED INTO
     *                          @p mediumVariant by the caller, because the two are independent axes of one
     *                          sky — the same code with a different tint is a different picture, and two
     *                          hashes folded together before they arrive can cancel each other exactly
     *                          once, which is a rebake that never happens and no way to find out why.
     * @return 0 exactly when @p marched is false; never 0 otherwise, so "no clouds" and "these clouds"
     *         cannot collide.
     */
    inline uint64_t CloudEnvironmentFingerprint( const CloudGpuPayload& payload, bool marched,
                                                 bool skyOcclusionValid, uint32_t shapeGeneration,
                                                 uint64_t mediumVariant, uint64_t mediumValues )
    {
        if ( !marched )
            return 0ull;

        CloudGpuPayload key = payload;

        key.Wind.x   = 0.0f;
        key.Wind.y   = 0.0f;
        key.Wind.z   = 0.0f;
        for ( glm::vec4& level : key.Level )
        {
            level.x = 0.0f;
            level.y = 0.0f;
        }
        for ( glm::vec4& band : key.FarPresence )
            band = glm::vec4( 0.0f );
        for ( glm::vec4& band : key.FarProfile )
            band = glm::vec4( 0.0f );

        // FNV-1a over the block's bytes. The block is packed to exactly 268 bytes with no padding (the
        // static_asserts in CloudPayload.hpp pin every offset), so there are no uninitialised holes for
        // the hash to read and two equal payloads always hash equal.
        uint64_t             hash  = 1469598103934665603ull;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>( &key );
        for ( size_t i = 0; i < sizeof( key ); ++i )
        {
            hash ^= static_cast<uint64_t>( bytes[i] );
            hash *= 1099511628211ull;
        }

        hash ^= skyOcclusionValid ? 0x9E3779B97F4A7C15ull : 0ull;

        hash ^= static_cast<uint64_t>( shapeGeneration );
        hash *= 1099511628211ull;

        hash ^= mediumVariant;
        hash *= 1099511628211ull;

        hash ^= mediumValues;
        hash *= 1099511628211ull;

        // Never zero: zero is reserved for "this view has no clouds", and a collision between that and a
        // real sky would leave an overcast baked into a scene whose layer was just deleted.
        return hash | 1ull;
    }
} // namespace Desert::Graphic

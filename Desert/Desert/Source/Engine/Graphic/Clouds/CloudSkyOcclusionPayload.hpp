#pragma once

#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    /**
     * The C++ half of the SKY-LIGHT OCCLUSION VOLUME: its extents, its bindings, and the bytes it costs.
     *
     * WHAT THE VOLUME IS, in one sentence, because nothing else here makes sense first: a 128 x 16 x 128
     * RGBA16F field over the SAME region the procedural modelling volume covers, whose every texel holds
     * the diffuse (cosine-weighted hemispherical) transmittance of all the cloud above that column at that
     * altitude. It is the quantity `CloudAmbientOcclusion` cannot express — Р0's ranked #1 and #2,
     * Docs/Clouds/DIAGNOSIS_CARTOON.md §1 — and Editor/Resources/Shaders/Common/CloudLighting.glslh owns
     * both the maths and the addressing.
     *
     * THE CONSTANTS BELOW ARE MIRRORS, on exactly the terms Graphic::CloudShadowPayload states for its
     * own: the statement of record is that header's `#define`s, this file restates them for the renderer,
     * and Desert/Tests/Engine/CloudLighting compiles the header as C++ so the two sides are asserted equal
     * rather than assumed. A mirror nobody checks is a mirror that drifts, and the symptom here would be a
     * volume dispatched at one size and addressed at another — a shaded side that slides across the sky.
     */

    // Texels per horizontal side, and altitude slices. HALF THE PROCEDURAL MODELLING VOLUME ON EVERY AXIS
    // (256 x 32 x 256), which is a relation and not a size: what this stores is a hemispherical integral
    // whose own horizontal footprint is the height of the cloud above the sample, so a texel finer than
    // the layer is thick would store structure the quantity does not have. See the header's own note.
    inline constexpr uint32_t kCloudSkyOcclusionResolution = 128u;
    inline constexpr uint32_t kCloudSkyOcclusionSlices     = 16u;

    /// The bytes one such volume costs on the device. RGBA16F is eight bytes a texel; only .r is written,
    /// because this engine's Core::Formats::ImageFormat has no one- or two-channel float format — the
    /// waste is stated here rather than discovered from a memory report.
    inline constexpr uint64_t kCloudSkyOcclusionBytes = static_cast<uint64_t>( kCloudSkyOcclusionResolution ) *
                                                        kCloudSkyOcclusionSlices * kCloudSkyOcclusionResolution *
                                                        8ull;

    // The volume's setter-bound buffers, the march's own slots (one vocabulary for one field). Its images are
    // binding-block entries named by shader name, the same names the march uses.
    inline constexpr uint32_t kCloudSkyOcclusionParamsBinding   = kCloudParamsBinding;
    inline constexpr uint32_t kCloudSkyOcclusionAuthoredBinding = kCloudAuthoredBinding;
} // namespace Desert::Graphic

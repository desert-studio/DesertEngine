#pragma once

#include <cstdint>

namespace Desert::Graphic
{
    // Must match the conversion shaders' LocalSize(32, 32, 1) — a dispatch derived with a different
    // group size under-covers the output and the right/bottom edge is never written.
    inline constexpr uint32_t kComputeImagesWorkGroupSize = 32u;
    // The cube programs' (PanoramaToCubemap, PrefilterEnvMap; recorded by System::SkyCapture) set-0 map: the source as a separate
    // texture at 0, the output storage image at 1, and the one shared state the source is read through at 2.
    // That state is trilinear REPEAT and not the global texture filter: a bake is cached by its inputs, and a
    // display setting must not change what it bakes. REPEAT wraps the panorama's longitude; a cube ignores it.
    inline constexpr uint32_t kComputeImagesInputBinding   = 0u;
    inline constexpr uint32_t kComputeImagesOutputBinding  = 1u;
    inline constexpr uint32_t kComputeImagesSamplerBinding = 2u;

    // Whole workgroups covering @p extentTexels threads: the pair (extent, groups) this returns is the
    // relation Tests/Engine/SkyRules pins — every texel covered, no whole surplus group. Deriving the
    // extent from anything but the output's own face is how one bake dispatched 12x its texel count.
    constexpr uint32_t DispatchGroupCount( uint32_t extentTexels, uint32_t localSize )
    {
        return ( extentTexels + localSize - 1u ) / localSize;
    }
} // namespace Desert::Graphic

#pragma once

#include <atomic>

namespace Desert::Graphic
{
    // WHAT THE VULKAN BACKEND HAS TO BE ABLE TO READ WITHOUT ASKING ANYBODY, and nothing else.
    //
    // Two kinds of value live here and they are not the same kind:
    //
    //   * DEVICE FACTS, discovered once at device init and never chosen by a person — MaxAnisotropy,
    //     MaxMSAASamples, WideLines. Nothing upstream can set these and nothing should try.
    //   * A DERIVED COPY of a user choice, kept atomic because sampler creation reads it off whichever
    //     thread is cooking a texture — TextureFilter and AnisotropyLevel. The AUTHORITY on both is
    //     Common::Settings::MachineSettings; SceneRenderer::BeginScene is the ONLY writer of the copy,
    //     and it recreates the samplers when it moves.
    //
    // `MSAASamples` USED TO BE A THIRD KIND and is gone: a copy of the user's MSAA choice, written by
    // Editor::EditorPreferences and read once by SceneRenderer::Init. Two things followed from that and
    // both were wrong. The renderer reads MachineSettings directly now, so the copy that could disagree
    // with its source does not exist, and the packaged game — which never opens editor.json and therefore
    // never had a writer for it — gets the sample count its player asked for.
    //
    // The `TextureFilterMode` ENUM used to be declared here as well, with `Core::TextureFilter` mirroring
    // it under a "Must match" comment and nothing asserting that it did. One declaration now, in
    // Common/Settings/MachineSettings.hpp, which is where the value is authored.
    struct RenderConfig
    {
        // Current global texture sampler filter, as Common::Settings::TextureFilter's underlying int
        // (VulkanImage::CreateSampler reads this).
        static inline std::atomic<int> TextureFilter{ 2 /* Trilinear */ };
        // Device max sampler anisotropy (0 = anisotropy unsupported). Set once at device init.
        static inline std::atomic<float> MaxAnisotropy{ 0.0f };
        // User-selected anisotropy level (1/2/4/8/16); clamped to MaxAnisotropy at sampler creation.
        static inline std::atomic<int> AnisotropyLevel{ 8 };

        // Device colour&depth sample caps, set at device init.
        static inline std::atomic<int> MaxMSAASamples{ 1 };

        // Device supports line widths > 1 (VkPhysicalDeviceFeatures.wideLines). MoltenVK does NOT —
        // setting a wider line then is a validation error, so the debug-line paths clamp to 1.0.
        static inline std::atomic<bool> WideLines{ false };
    };
} // namespace Desert::Graphic

#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <atomic>

namespace Desert::Graphic
{
    // WHAT THE VULKAN BACKEND HAS TO BE ABLE TO READ WITHOUT ASKING ANYBODY, and nothing else.
    //
    // Two kinds of value live here and they are not the same kind:
    //
    //   * DEVICE FACTS, discovered once at device init and never chosen by a person — WideLines. Nothing
    //     upstream can set these and nothing should try. (MaxAnisotropy / MaxMSAASamples lived here until
    //     SCAL1: the device's anisotropy levels and MSAA counts are CapabilityCatalog lists now, and
    //     Scalability::Resolve narrows a choice to them before anything reads it.)
    //   * A DERIVED COPY of a user choice, kept atomic because sampler creation reads it off whichever
    //     thread is cooking a texture — TextureFilter and AnisotropyLevel. The AUTHORITY on both is
    //     Common::Scalability::QualityState (ResolvedQuality); QualityBoot's listener is the ONLY writer of
    //     the copy, and it recreates the samplers when it moves.
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
        // Resolved anisotropy level (1/2/4/8/16), already one of CapabilityCatalog::AnisotropyLevels — the
        // resolver narrowed it to the device, so sampler creation uses it as is.
        static inline std::atomic<int> AnisotropyLevel{ 8 };
        // Resolved Textures.MipBias in 1/100 mip (Low +100, Medium +50, High and above 0): the level-of-detail
        // bias every engine image's own sampler carries (GlobalTextureFilterSampler). Same writer as the two
        // above.
        static inline std::atomic<int> TextureMipBias{ 0 };
        // Resolved Effects.ParticleSpawnPercent (25..100): every scene's VFXWorld reads it before its Tick
        // (Scene::OnUpdate -> VFXWorld::SetSpawnCountScale). Global like the sampler state, because the quality
        // is per machine while the effects world is per scene.
        static inline std::atomic<int> ParticleSpawnPercent{ 100 };

        // Device supports line widths > 1 (VkPhysicalDeviceFeatures.wideLines). MoltenVK does NOT —
        // setting a wider line then is a validation error, so the debug-line paths clamp to 1.0.
        static inline std::atomic<bool> WideLines{ false };

        // `--cloud-visualize N` (Editor command line): the cloud march writes a diagnostic instead of the
        // sky, read per dispatch into CloudPush::Visualize. 0 = off. 1 = the cut at the ray's entry into the
        // layer: R the local cover the weather leaves the column, G the lowest rank over eight heights of
        // the column, B 1 where the column is kept (G < R). 2 = along the ray: R the share of 32 samples the
        // cut keeps, G the largest rank step between neighbours x 8 (a seam reads bright). 3 = which clip
        // level of the modelling volume answers at the entry (0 red, 1 green, 2 blue; blend bands mixed).
        static inline std::atomic<int> CloudVisualize{ 0 };
    };

    // The sampler an engine Image2D / ImageCube carries as its own (VulkanImage CreateSampler, Global policy):
    // filter and mip mode from the global texture filter, REPEAT on every axis. The one derivation of it -- the
    // backend's own sampler reads it, and a graph entry that must read an image exactly as the image's own
    // sampler did (RDG-FAULT1 sampler parity) names it. Anisotropy is not part of a SamplerDesc: the entries
    // that name this sample from compute with an explicit level of detail, where anisotropy does not apply. The
    // mip bias IS part of it: Vulkan adds mipLodBias to an explicit level of detail too, so a graph read without
    // it would land on a different mip than the image's own sampler.
    inline RDG::SamplerDesc GlobalTextureFilterSampler()
    {
        using FM                      = Common::Settings::TextureFilter;
        const int                mode = RenderConfig::TextureFilter.load();
        const RDG::SamplerFilter filter =
             mode == static_cast<int>( FM::Nearest ) ? RDG::SamplerFilter::Nearest : RDG::SamplerFilter::Linear;
        const RDG::SamplerMipMode mip =
             mode == static_cast<int>( FM::Trilinear ) || mode == static_cast<int>( FM::Anisotropic )
                  ? RDG::SamplerMipMode::Linear
                  : RDG::SamplerMipMode::Nearest;
        const RDG::SamplerDesc desc{ filter,
                                     filter,
                                     mip,
                                     RDG::SamplerAddress::Repeat,
                                     RDG::SamplerAddress::Repeat,
                                     RDG::SamplerAddress::Repeat };
        return desc.WithMipLodBias( static_cast<int16_t>( RenderConfig::TextureMipBias.load() ) );
    }

    // The sampler an engine Image3D carries as its own (AlwaysLinear policy): LINEAR, linear mip, REPEAT, whatever
    // the global filter -- a volume's interpolation is part of the algorithm that samples it.
    constexpr RDG::SamplerDesc VolumeSampler()
    {
        return RDG::SamplerDesc::LinearRepeat();
    }
} // namespace Desert::Graphic

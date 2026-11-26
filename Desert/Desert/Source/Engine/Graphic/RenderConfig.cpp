#include <Engine/Graphic/RenderConfig.hpp>

namespace Desert::Graphic
{
    RDG::SamplerDesc GlobalTextureFilterSampler()
    {
        using FM                      = Common::Settings::TextureFilter;
        const int                mode = RenderConfig::TextureFilter.load();
        const RDG::SamplerFilter filter =
             mode == static_cast<int>( FM::Nearest ) ? RDG::SamplerFilter::Nearest : RDG::SamplerFilter::Linear;
        const RDG::SamplerMipMode mip =
             mode == static_cast<int>( FM::Trilinear ) || mode == static_cast<int>( FM::Anisotropic )
                  ? RDG::SamplerMipMode::Linear
                  : RDG::SamplerMipMode::Nearest;
        return { filter,
                 filter,
                 mip,
                 RDG::SamplerAddress::Repeat,
                 RDG::SamplerAddress::Repeat,
                 RDG::SamplerAddress::Repeat };
    }

    std::atomic<int> RenderConfig::TextureFilter{ 2 /* Trilinear */ };

    std::atomic<int> RenderConfig::AnisotropyLevel{ 8 };

    std::atomic<bool> RenderConfig::WideLines{ false };

    std::atomic<int> RenderConfig::CloudVisualize{ 0 };

    std::atomic<bool> RenderConfig::RdgCompileCache{ true };

    std::atomic<int> RenderConfig::ParallelSetupOverride{ -1 };
}

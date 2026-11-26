#include <Engine/Graphic/RenderConfig.hpp>

namespace Desert::Graphic
{
    RDG::SamplerDesc GlobalTextureFilterSampler()
    {
        using FM                      = Common::Settings::TextureFilter;
        const int                mode = RenderConfig::TextureFilter();
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

    std::atomic<int>  RenderConfig::s_TextureFilter{ 2 /* Trilinear */ };
    std::atomic<int>  RenderConfig::s_AnisotropyLevel{ 8 };
    std::atomic<bool> RenderConfig::s_WideLines{ false };
    std::atomic<int>  RenderConfig::s_CloudVisualize{ 0 };
    std::atomic<bool> RenderConfig::s_RdgCompileCache{ true };
    std::atomic<int>  RenderConfig::s_ParallelSetupOverride{ -1 };

    int RenderConfig::TextureFilter()
    {
        return s_TextureFilter.load();
    }

    int RenderConfig::AnisotropyLevel()
    {
        return s_AnisotropyLevel.load();
    }

    bool RenderConfig::SetTextureFilter( const int filter )
    {
        return s_TextureFilter.exchange( filter ) != filter;
    }

    bool RenderConfig::SetAnisotropyLevel( const int anisotropy )
    {
        return s_AnisotropyLevel.exchange( anisotropy ) != anisotropy;
    }

    bool RenderConfig::WideLines()
    {
        return s_WideLines.load();
    }

    void RenderConfig::SetWideLines( const bool supported )
    {
        s_WideLines.store( supported );
    }

    int RenderConfig::CloudVisualize()
    {
        return s_CloudVisualize.load();
    }

    void RenderConfig::SetCloudVisualize( const int mode )
    {
        s_CloudVisualize.store( mode );
    }

    bool RenderConfig::RdgCompileCache()
    {
        return s_RdgCompileCache.load( std::memory_order_relaxed );
    }

    void RenderConfig::SetRdgCompileCache( const bool enabled )
    {
        s_RdgCompileCache.store( enabled );
    }

    int RenderConfig::ParallelSetupOverride()
    {
        return s_ParallelSetupOverride.load();
    }

    void RenderConfig::SetParallelSetupOverride( const int value )
    {
        s_ParallelSetupOverride.store( value );
    }
} // namespace Desert::Graphic

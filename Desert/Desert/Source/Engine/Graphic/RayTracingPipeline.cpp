#include <Engine/Graphic/RayTracingPipeline.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>
#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic
{
    bool RayTracingReorderOffered()
    {
        const auto& catalog = EngineContext::GetInstance().GetCapabilities().Catalog;
        return Common::Scalability::CapabilityCatalog::Offers(
             catalog.RayTracingFeatures, Common::Scalability::RayTracingFeature::InvocationReorder );
    }

    Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
    RayTracingPipeline::Create( const RayTracingPipelineSpecification& spec )
    {
        using Result = std::shared_ptr<RayTracingPipeline>;
        if ( !spec.Program )
            return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': no ray generation program",
                                                       spec.DebugName );
        if ( spec.HitGroups.empty() )
            return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': no hit group", spec.DebugName );
        return Renderer::GetInstance().Rhi().CreateRayTracingPipeline( spec );
    }

    void RayTracingPipeline::Retire( std::shared_ptr<RayTracingPipeline>&& outgoing ) noexcept
    {
        if ( !outgoing )
            return;
        const auto ctx = EngineContext::GetInstance().GetRendererContext();
        if ( !ctx )
            return; // `outgoing` dies here
        ctx->RetireAfterFramesInFlight( [held = std::move( outgoing )]() mutable { held.reset(); } );
    }
} // namespace Desert::Graphic

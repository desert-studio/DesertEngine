#include <Engine/Graphic/RayTracingPipeline.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingPipeline.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic
{
    Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
    RayTracingPipeline::Create( const RayTracingPipelineSpecification& spec )
    {
        using Result = std::shared_ptr<RayTracingPipeline>;
        if ( !spec.Program )
            return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': no ray generation program",
                                                       spec.DebugName );
        if ( spec.HitGroups.empty() )
            return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': no hit group", spec.DebugName );
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': no rendering API is selected",
                                                           spec.DebugName );
            case RendererAPIType::Vulkan:
            {
                auto pipeline = std::make_shared<API::Vulkan::VulkanRayTracingPipeline>( spec );
                if ( const Common::BoolResultStr built = pipeline->Build(); !built )
                    return Common::MakeError<Result>( built.GetError() );
                return Common::MakeSuccess<Result>( std::move( pipeline ) );
            }
        }
        return Common::MakeFormattedError<Result>( "RayTracingPipeline '{}': unknown rendering API",
                                                   spec.DebugName );
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

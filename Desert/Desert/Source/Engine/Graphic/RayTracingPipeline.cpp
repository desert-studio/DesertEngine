#include <Engine/Graphic/RayTracingPipeline.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingPipeline.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

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
} // namespace Desert::Graphic

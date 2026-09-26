#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>

namespace Desert::Graphic
{
    Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
    GraphicsPipeline::Create( const GraphicsPipelineSpecification& spec )
    {
        // ASKED BEFORE ANYTHING IS CONSTRUCTED. The leaf's Invalidate dereferences spec.Shader to read
        // its descriptor set layouts, and threw std::runtime_error on a missing framebuffer — both are
        // process death, which is the outcome the caller is being handed a Result in order to avoid.
        if ( const auto buildable = CheckGraphicsPipelineSpecification( spec ); !buildable )
        {
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( buildable.GetError() );
        }

        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                // A REFUSAL WITH A REASON, not a null nobody could tell apart from "the shader was bad".
                return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                     "GraphicsPipeline '" + spec.DebugName + "': no rendering API is selected." );
            case RendererAPIType::Vulkan:
            {
                auto pipeline = std::make_shared<API::Vulkan::VulkanPipeline>( spec );
                pipeline->Invalidate();

                // THE HANDLE, NOT A FLAG. Invalidate's own refusal (a program with no vertex stage,
                // e.g. a compute shader reached by name) leaves the VkPipeline null, and asking the
                // object what it actually built is the one question that cannot drift away from what it
                // did — a second `bool m_Built` beside it would be exactly the mirror this engine keeps
                // finding out of step with its subject.
                if ( pipeline->GetVkPipeline() == VK_NULL_HANDLE )
                {
                    return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                         "GraphicsPipeline '" + spec.DebugName +
                         "': the Vulkan pipeline was not built (see the error above)." );
                }
                return Common::MakeSuccess<std::shared_ptr<GraphicsPipeline>>( std::move( pipeline ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( "Unknown RenderingAPI" );
    }

    Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
    GraphicsPipeline::CreateAsync( const GraphicsPipelineSpecification& spec )
    {
        if ( const auto buildable = CheckGraphicsPipelineSpecification( spec ); !buildable )
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( buildable.GetError() );

        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                     "GraphicsPipeline '" + spec.DebugName + "': no rendering API is selected." );
            case RendererAPIType::Vulkan:
            {
                auto pipeline = std::make_shared<API::Vulkan::VulkanPipeline>( spec );
                pipeline->InvalidateAsync();
                // UNBUILT here means the leaf refused before any compile was handed out (its reason is
                // logged); Compiling/Built/Failed all mean the driver has it.
                if ( pipeline->GetBuildState() == API::Vulkan::VulkanPipeline::BuildState::Unbuilt )
                {
                    return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                         "GraphicsPipeline '" + spec.DebugName +
                         "': the Vulkan pipeline was not built (see the error above)." );
                }
                return Common::MakeSuccess<std::shared_ptr<GraphicsPipeline>>( std::move( pipeline ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( "Unknown RenderingAPI" );
    }

    Common::ResultStr<std::shared_ptr<ComputePipeline>>
    ComputePipeline::Create( const ComputePipelineSpecification& spec )
    {
        // ASKED BEFORE ANYTHING IS CONSTRUCTED, because the leaf's constructor already dereferences
        // spec.Shader (it builds a VulkanMaterialBackend from it). The `DESERT_VERIFY( spec.Shader )`
        // that stood here was not a refusal at all: it logs and then takes the process down, which is
        // the outcome the caller is being handed a Result in order to avoid.
        if ( const auto buildable = CheckComputePipelineSpecification( spec ); !buildable )
        {
            return Common::MakeError<std::shared_ptr<ComputePipeline>>( buildable.GetError() );
        }

        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                // A REFUSAL WITH A REASON, not a null nobody could tell apart from "the shader was bad".
                return Common::MakeError<std::shared_ptr<ComputePipeline>>(
                     std::string( "ComputePipeline '" ) + spec.DebugName + "': no rendering API is selected." );
            case RendererAPIType::Vulkan:
            {
                auto pipeline = std::make_shared<API::Vulkan::VulkanPipelineCompute>( spec );
                pipeline->Invalidate();

                // THE HANDLE, NOT A FLAG. Invalidate's own refusal (a shader with no compute stage in
                // it) leaves the VkPipeline null, and asking the object what it actually built is the
                // one question that cannot drift away from what it did — a second `bool m_Built` beside
                // it would be exactly the mirror this engine keeps finding out of step with its subject.
                if ( pipeline->GetVkPipeline() == VK_NULL_HANDLE )
                {
                    return Common::MakeError<std::shared_ptr<ComputePipeline>>(
                         std::string( "ComputePipeline '" ) + spec.DebugName +
                         "': the Vulkan pipeline was not built (see the error above)." );
                }
                return Common::MakeSuccess<std::shared_ptr<ComputePipeline>>( std::move( pipeline ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return Common::MakeError<std::shared_ptr<ComputePipeline>>( "Unknown RenderingAPI" );
    }

} // namespace Desert::Graphic

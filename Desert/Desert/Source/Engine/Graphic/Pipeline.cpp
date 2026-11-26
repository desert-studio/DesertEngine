#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

#include <algorithm>
#include <atomic>

namespace Desert::Graphic
{
    namespace
    {
        std::atomic<uint64_t> g_PipelineRebuildEpoch{ 0 };
    } // namespace

    uint64_t PipelineRebuildEpoch()
    {
        return g_PipelineRebuildEpoch.load( std::memory_order_acquire );
    }

    bool AnyPipelineBehindItsShader()
    {
        const std::lock_guard lock( detail::LivePipelines().Mutex );
        return std::ranges::any_of( detail::LivePipelines().All,
                                    []( const IPipeline* pipeline ) { return pipeline->IsBehindItsShader(); } );
    }

    size_t RebuildPipelinesBehindTheirShader()
    {
        // Under the lock for the whole pass: a pipeline destroyed on another thread mid-pass would otherwise be
        // rebuilt after its destructor ran. A rebuild creates no pipeline objects, so it never re-enters here.
        const std::lock_guard lock( detail::LivePipelines().Mutex );
        size_t                rebuilt = 0;
        for ( IPipeline* pipeline : detail::LivePipelines().All )
        {
            if ( !pipeline->IsBehindItsShader() )
                continue;
            pipeline->Invalidate(); // records the new generation; a refusal logs its reason and leaves it unbuilt
            ++rebuilt;
        }
        if ( rebuilt > 0 )
            g_PipelineRebuildEpoch.fetch_add( 1, std::memory_order_acq_rel );
        return rebuilt;
    }

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
        return Renderer::GetInstance().Rhi().CreateGraphicsPipeline( spec );
    }

    Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
    GraphicsPipeline::CreateAsync( const GraphicsPipelineSpecification& spec, const PipelineRole role )
    {
        if ( const auto buildable = CheckGraphicsPipelineSpecification( spec ); !buildable )
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( buildable.GetError() );
        return Renderer::GetInstance().Rhi().CreateGraphicsPipelineAsync( spec, role );
    }

    Common::ResultStr<std::shared_ptr<ComputePipeline>>
    ComputePipeline::Create( const ComputePipelineSpecification& spec )
    {
        // ASKED BEFORE ANYTHING IS CONSTRUCTED, because the backend's constructor already dereferences
        // spec.Shader (it builds a material backend from it).
        if ( const auto buildable = CheckComputePipelineSpecification( spec ); !buildable )
        {
            return Common::MakeError<std::shared_ptr<ComputePipeline>>( buildable.GetError() );
        }
        return Renderer::GetInstance().Rhi().CreateComputePipeline( spec );
    }

    std::atomic<uint64_t>& PipelineReadinessCounter()
    {
        static std::atomic<uint64_t> counter{ 0 };
        return counter;
    }
}

namespace Desert::Graphic::detail
{
    LivePipelineSet& LivePipelines()
    {
        static LivePipelineSet s_Live;
        return s_Live;
    }
}

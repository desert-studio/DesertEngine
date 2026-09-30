#include "RenderGraphBuilder.hpp"
#include "RenderPhaseRegistry.hpp"
#include <algorithm>

namespace Desert::Graphic
{
    RenderGraphBuilder::RenderGraphBuilder()
    {
    }

    RenderGraphBuilder::~RenderGraphBuilder()
    {
    }

    RenderGraphBuilder::PassConfig& RenderGraphBuilder::AddPass( const PassConfig& config )
    {
        PassConfig stored        = config;
        stored.RegistrationIndex = m_NextRegistrationIndex++;

        LOG_DEBUG( "Added pass '{}' to phase '{}' (order {}, registration #{}) with {} dependencies", stored.Name,
                   RenderPhaseToString( stored.Phase ), stored.OrderInPhase, stored.RegistrationIndex,
                   stored.Dependencies.size() );

        std::vector<PassConfig>& phase = m_PhasePasses[stored.Phase];
        phase.push_back( std::move( stored ) );
        return phase.back();
    }

    RenderGraphBuilder::PassConfig& RenderGraphBuilder::AddPass(
         const std::string& name, RenderPhaseID phase, std::function<void()> executeFunc,
         const GraphicsPipelineSpecification& pipelineSpec, std::shared_ptr<Framebuffer> targetFramebuffer,
         const std::vector<RenderPassDependency>& dependencies, const std::optional<glm::vec4>& clearColor,
         int32_t orderInPhase, const std::optional<float>& clearDepth )
    {
        PassConfig config;
        config.Name              = name;
        config.Phase             = phase;
        config.ExecuteFunc       = executeFunc;
        config.PipelineSpec      = pipelineSpec;
        config.TargetFramebuffer = targetFramebuffer;
        config.Dependencies      = dependencies;
        config.ClearColor        = clearColor;
        config.OrderInPhase      = orderInPhase;
        config.ClearDepth        = clearDepth;

        return AddPass( config );
    }

    void RenderGraphBuilder::AddPhaseDependency( RenderPhaseID requiredPhase, RenderPhaseID dependentPhase )
    {
        m_PhaseDependencies[dependentPhase].insert( requiredPhase );
    }

    void RenderGraphBuilder::AddTextureDependency( const std::string& textureName,
                                                   RenderPhaseID      producerPhase,
                                                   RenderPhaseID      consumerPhase )
    {
        m_TextureDependencies[textureName] = { producerPhase, consumerPhase };
        AddPhaseDependency( producerPhase, consumerPhase );
    }

    bool RenderGraphBuilder::Build()
    {
        if ( !ValidateDependencies() )
        {
            LOG_ERROR( "RenderGraph validation failed!" );
            return false;
        }

        TopologicalSort();
        return true;
    }

    bool RenderGraphBuilder::ValidateDependencies() const
    {
        if ( CheckForCycles() )
        {
            LOG_ERROR( "Cyclic dependencies detected in render graph!" );
            return false;
        }

        for ( const auto& [textureName, phases] : m_TextureDependencies )
        {
            if ( phases.first == RenderPhase::None || phases.second == RenderPhase::None )
            {
                LOG_ERROR( "Texture '{}' has invalid phase dependencies", textureName );
                return false;
            }
        }

        return true;
    }

    void RenderGraphBuilder::TopologicalSort()
    {
        // Both levels of the order are decided by the pure functions in RenderGraphSort.hpp; this
        // function only feeds them and then does the one thing they cannot: create the GPU render pass
        // objects. Keeping the decision out of here is what makes it testable at all — see the header.

        std::set<RenderPhaseID> presentPhases;
        for ( const auto& [phase, passes] : m_PhasePasses )
            presentPhases.insert( phase );

        const auto& declOrder = RenderPhaseRegistry::GetInstance().GetDeclarationOrder();
        m_PhaseOrder          = OrderRenderPhases( presentPhases, m_PhaseDependencies, declOrder );

        // An unregistered phase ID used to lose its passes without a word — the sort simply never
        // reached them. They are placed at the end now, and the mistake is named out loud, because
        // "my pass never ran" is otherwise indistinguishable from "my pass drew nothing".
        for ( RenderPhaseID phase : presentPhases )
        {
            if ( std::find( declOrder.begin(), declOrder.end(), phase ) == declOrder.end() )
            {
                LOG_ERROR( "Render graph: phase ID {} owns {} pass(es) but was never registered with "
                           "RenderPhaseRegistry; it is ordered last. Register it before building.",
                           phase, m_PhasePasses[phase].size() );
            }
        }

        std::vector<RenderPassOrderKey> keys;
        std::vector<PassConfig*>        passes;
        for ( auto& [phase, phasePasses] : m_PhasePasses )
        {
            for ( auto& pass : phasePasses )
            {
                keys.push_back( RenderPassOrderKey{ pass.Phase, pass.OrderInPhase, pass.RegistrationIndex } );
                passes.push_back( &pass );
            }
        }

        m_SortedPasses.clear();
        m_SortedPasses.reserve( passes.size() );
        for ( std::size_t index : OrderRenderPasses( keys, m_PhaseOrder ) )
        {
            PassConfig& pass = *passes[index];
            if ( pass.TargetFramebuffer && !pass.CachedRenderPass )
            {
                RenderPassSpecification rpSpec;
                rpSpec.TargetFramebuffer = pass.TargetFramebuffer;
                rpSpec.DebugName         = pass.Name;
                if ( pass.ClearColor )
                    rpSpec.ClearColor.Color = *pass.ClearColor;
                if ( pass.ClearDepth )
                    rpSpec.ClearColor.DepthStencil.x = *pass.ClearDepth;
                pass.CachedRenderPass = RenderPass::Create( rpSpec );
            }
            m_SortedPasses.push_back( pass );
        }
    }

    bool RenderGraphBuilder::CheckForCycles() const
    {
        std::unordered_map<RenderPhaseID, std::vector<RenderPhaseID>> graph;
        std::unordered_map<RenderPhaseID, int>                        visited;

        for ( const auto& [phase, deps] : m_PhaseDependencies )
        {
            for ( RenderPhaseID dep : deps )
                graph[phase].push_back( dep );
        }

        std::function<bool( RenderPhaseID )> hasCycle = [&]( RenderPhaseID phase )
        {
            if ( visited[phase] == 1 )
                return true;
            if ( visited[phase] == 2 )
                return false;

            visited[phase] = 1;
            for ( RenderPhaseID neighbor : graph[phase] )
            {
                if ( hasCycle( neighbor ) )
                    return true;
            }
            visited[phase] = 2;
            return false;
        };

        for ( const auto& [phase, _] : graph )
        {
            if ( hasCycle( phase ) )
                return true;
        }

        return false;
    }

    const std::vector<RenderGraphBuilder::PassConfig>& RenderGraphBuilder::GetSortedPasses() const
    {
        return m_SortedPasses;
    }

    void RenderGraphBuilder::Clear()
    {
        m_PhasePasses.clear();
        m_PhaseOrder.clear();
        m_SortedPasses.clear();
        m_PhaseDependencies.clear();
        m_TextureDependencies.clear();

        // Registration numbers restart with the graph. A rebuild registers the same passes again, so
        // reusing the counter would only make the numbers in the log grow without changing the order.
        m_NextRegistrationIndex = 0;

        LOG_DEBUG( "Render graph builder cleared" );
    }

} // namespace Desert::Graphic

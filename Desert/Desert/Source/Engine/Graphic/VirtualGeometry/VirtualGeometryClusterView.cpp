#include <Engine/Graphic/VirtualGeometry/VirtualGeometryClusterView.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Geometry/ClusterDag.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <cstddef>
#include <utility>

namespace Desert::Graphic::VirtualGeometry
{
    namespace
    {
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct ClusterViewShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, VgClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgDags );
        };

        // ClusterViewPush of the shader.
        struct ClusterViewPush
        {
            glm::mat4 WorldViewProjection{ 1.0f };
            uint32_t  FirstCluster = 0;
            uint32_t  ClusterCount = 0;
        };
        static_assert( sizeof( ClusterViewPush ) == 72 );

        // The shader reads a vertex's position as the first three floats of GpuClusterDag::VertexStride bytes.
        static_assert( offsetof( Vertex, Position ) == 0, "Common/VirtualGeometry.glslh VgClusterCornerPosition" );
    } // namespace

    bool ClusterView::Ensure()
    {
        if ( m_Pipeline )
            return true;
        if ( m_Refused )
            return false;
        m_Refused = true; // until the pipeline exists: one log line, not one per frame
        m_Shader  = Runtime::ResourceRegistry::GetShaderService()->GetByName( "VirtualGeometryClusters" );
        if ( !m_Shader )
        {
            LOG_ERROR( "[VirtualGeometry] the vg_clusters view is off: shader VirtualGeometryClusters not found" );
            return false;
        }
        GraphicsPipelineSpecification spec;
        spec.DebugName         = "VirtualGeometryClustersPipeline";
        spec.Shader            = m_Shader;
        spec.TargetLayout      = SceneTargetLayout();
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = false;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.CullMode          = CullMode::None;
        const auto pipeline    = GraphicsPipeline::Create( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[VirtualGeometry] the vg_clusters view is off: {}", pipeline.GetError() );
            return false;
        }
        m_Pipeline = pipeline.GetValue();
        m_Refused  = false;
        return true;
    }

    void ClusterView::Declare( RDG::PassBuilder& pass, const RDG::BufferRef clusters, const RDG::BufferRef dags,
                               std::vector<ClusterViewDraw> draws )
    {
        m_Draws = std::move( draws );
        if ( !m_Pipeline || m_Draws.empty() || !clusters.IsValid() || !dags.IsValid() )
        {
            m_Draws.clear();
            return;
        }
        (void)pass.Bindings( m_Layout.Get( m_Shader ), RDG::OtherRouteFill{} )
             .Storage( ClusterViewShaderParameters::VgClusters, clusters, RDG::Access::StorageRead )
             .Storage( ClusterViewShaderParameters::VgDags, dags, RDG::Access::StorageRead )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( ClusterViewPush ) ) );
    }

    Common::BoolResultStr ClusterView::Record( const RDG::PassContext& context ) const
    {
        if ( m_Draws.empty() || !m_Pipeline )
            return BOOLSUCCESS;
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        for ( const ClusterViewDraw& draw : m_Draws )
        {
            const ClusterViewPush push{ .WorldViewProjection = draw.WorldViewProjection,
                                        .FirstCluster        = draw.FirstCluster,
                                        .ClusterCount        = draw.ClusterCount };
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            if ( auto drawn = Renderer::DrawProcedural( bindings, *m_Pipeline, nullptr,
                                                        Geometry::kClusterMaxTriangles * 3u, draw.ClusterCount );
                 !drawn )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::VirtualGeometry
#endif // DESERT_DEV_INSTRUMENTS

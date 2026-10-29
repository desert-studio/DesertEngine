#include <Engine/Graphic/VirtualGeometry/VirtualGeometryClusterView.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <cstddef>

namespace Desert::Graphic::VirtualGeometry
{
    namespace
    {
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct ClusterViewShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, VgClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgDags );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgPrimitives );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgPassPrimitiveDraws );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgPassDrawSlots );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgSubmeshTransforms );
            DESERT_SHADER_PARAMETER( StorageBuffer, VgVisibleClusters );
        };

        // ClusterViewPush of the shader.
        struct ClusterViewPush
        {
            glm::mat4 ViewProjection{ 1.0f };
            uint32_t  VisibleCapacity = 0;
            uint32_t  Pad0            = 0;
            uint32_t  Pad1            = 0;
            uint32_t  Pad2            = 0;
        };
        static_assert( sizeof( ClusterViewPush ) == 80 );

        // GpuClusterRasterArgs::HwDraw: the VkDrawIndirectCommand the cut counted its HW clusters into.
        constexpr uint64_t kHwDrawOffset = offsetof( GpuClusterRasterArgs, HwDraw );
        static_assert( kHwDrawOffset == 16u, "Common/VirtualGeometry.glslh VgRasterArgs" );

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

    void ClusterView::Declare( RDG::PassBuilder& pass, const ClusterViewDraw& draw )
    {
        m_Draw     = draw;
        m_Declared = m_Pipeline && draw.VisibleCapacity != 0u && draw.Clusters.IsValid() && draw.Dags.IsValid() &&
                     draw.Primitives.IsValid() && draw.Instances.IsValid() && draw.PrimitiveDraws.IsValid() &&
                     draw.DrawSlots.IsValid() && draw.SubmeshTransforms.IsValid() &&
                     draw.VisibleClusters.IsValid() && draw.RasterArgs.IsValid();
        if ( !m_Declared )
            return;
        using P          = ClusterViewShaderParameters;
        constexpr auto R = RDG::Access::StorageRead;
        pass.Read( draw.RasterArgs, RDG::Access::IndirectArgs );
        (void)pass.Bindings( m_Layout.Get( m_Shader ), RDG::OtherRouteFill{} )
             .Storage( P::VgClusters, draw.Clusters, R )
             .Storage( P::VgDags, draw.Dags, R )
             .Storage( P::VgPrimitives, draw.Primitives, R )
             .Storage( P::VgInstances, draw.Instances, R )
             .Storage( P::VgPassPrimitiveDraws, draw.PrimitiveDraws, R )
             .Storage( P::VgPassDrawSlots, draw.DrawSlots, R )
             .Storage( P::VgSubmeshTransforms, draw.SubmeshTransforms, R )
             .Storage( P::VgVisibleClusters, draw.VisibleClusters, R )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( ClusterViewPush ) ) );
    }

    Common::BoolResultStr ClusterView::Record( const RDG::PassContext& context ) const
    {
        if ( !m_Declared || !m_Pipeline )
            return BOOLSUCCESS;
        RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
        const ClusterViewPush push{ .ViewProjection = m_Draw.ViewProjection, .VisibleCapacity = m_Draw.VisibleCapacity };
        bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
        // vertexCount = kClusterMaxTriangles x 3 (the cut wrote it, VirtualGeometryRenderer.cpp RasterArgs).
        return Renderer::DrawProceduralIndirect( bindings, *m_Pipeline, nullptr, m_Draw.RasterArgs, kHwDrawOffset );
    }
} // namespace Desert::Graphic::VirtualGeometry
#endif // DESERT_DEV_INSTRUMENTS

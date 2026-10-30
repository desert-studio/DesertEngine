#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic
{
    class GraphicsPipeline;
    class Shader;
} // namespace Desert::Graphic

namespace Desert::Graphic::RDG
{
    class PassBuilder;
    class PassContext;
} // namespace Desert::Graphic::RDG

// THE vg_clusters VIEW (DebugViewState::VirtualGeometry = VirtualGeometryView::Clusters; UE r.Nanite.Visualize
// Clusters): "Debug: Virtual Geometry Clusters" draws the clusters THIS FRAME'S CLUSTER CUT chose
// (SceneRenderer::AddFrameVirtualGeometry -> VirtualGeometryRenderer::AddClusterCull) over the lit scene, one
// hashed colour per cluster row (Programs/Debug/VirtualGeometryClusters.shader): ONE indirect draw whose
// instance count is the cut's RasterArgs.HwDraw[1]; instance i is the HW cluster VisibleClusters[VisibleCapacity
// - 1 - i] at the world the cut used (primitive x instance x the view pass's lod-0 submesh transform). The proof
// that the cut, the cooked DAG, the pool words and the table rows agree with the mesh the scene draws.
namespace Desert::Graphic::VirtualGeometry
{
    // What the view draws from: the cut's output and the tables it resolved the world through.
    struct ClusterViewDraw
    {
        RDG::BufferRef Clusters;          // VirtualMeshBuffers::Clusters
        RDG::BufferRef Dags;              // VirtualMeshBuffers::Dags
        RDG::BufferRef Primitives;        // GpuSceneBuffers::Primitives
        RDG::BufferRef Instances;         // GpuSceneBuffers::Instances
        RDG::BufferRef PrimitiveDraws;    // the cut view pass's PassDrawBuffers::PrimitiveDraws
        RDG::BufferRef DrawSlots;         // ...::DrawSlots
        RDG::BufferRef SubmeshTransforms; // DrawCommandBuffers::SubmeshTransforms
        RDG::BufferRef VisibleClusters;   // ClusterCullResult::VisibleClusters
        RDG::BufferRef RasterArgs;        // ClusterCullResult::RasterArgs (the HwDraw record at byte 16)
        uint32_t       VisibleCapacity = 0;
        glm::mat4      ViewProjection{ 1.0f }; // the view's jittered view-projection
    };

    class ClusterView
    {
    public:
        // The shader and the pipeline (scene target layout, CloserOrEqual, no depth write), made once; a refusal
        // is logged once and the view stays off.
        [[nodiscard]] bool Ensure();
        // Declares the pass's block (every table of @p draw, the args as IndirectArgs, the push range) and keeps
        // @p draw for Record. A draw with an invalid table records nothing.
        void                                Declare( RDG::PassBuilder& pass, const ClusterViewDraw& draw );
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const;

    private:
        std::shared_ptr<Shader>           m_Shader;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        ShaderBindingLayoutCache          m_Layout;
        ClusterViewDraw                   m_Draw;
        bool                              m_Declared = false;
        bool                              m_Refused  = false;
    };
} // namespace Desert::Graphic::VirtualGeometry
#endif // DESERT_DEV_INSTRUMENTS

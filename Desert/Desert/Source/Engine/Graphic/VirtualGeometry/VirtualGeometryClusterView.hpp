#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

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
// Clusters): "Debug: Virtual Geometry Clusters" re-draws the level-0 clusters of every drawn virtual-geometry
// submesh from the VirtualMeshTable rows over the lit scene, one hashed colour per cluster row
// (Programs/Debug/VirtualGeometryClusters.shader). The proof that the cooked DAG, the pool words and the table
// rows agree with the mesh the scene draws: a wrong address or offset shows as a torn or missing surface.
namespace Desert::Graphic::VirtualGeometry
{
    // One submesh DAG drawn: instance i = Clusters[FirstCluster + i].
    struct ClusterViewDraw
    {
        glm::mat4 WorldViewProjection{ 1.0f };
        uint32_t  FirstCluster = 0;
        uint32_t  ClusterCount = 0;
    };

    class ClusterView
    {
    public:
        // The shader and the pipeline (scene target layout, CloserOrEqual, no depth write), made once; a refusal
        // is logged once and the view stays off.
        [[nodiscard]] bool Ensure();
        // Declares the pass's block (the Clusters and Dags tables, the push range) and keeps @p draws for Record.
        void Declare( RDG::PassBuilder& pass, RDG::BufferRef clusters, RDG::BufferRef dags,
                      std::vector<ClusterViewDraw> draws );
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const;

    private:
        std::shared_ptr<Shader>           m_Shader;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        ShaderBindingLayoutCache          m_Layout;
        std::vector<ClusterViewDraw>      m_Draws;
        bool                              m_Refused = false;
    };
} // namespace Desert::Graphic::VirtualGeometry
#endif // DESERT_DEV_INSTRUMENTS

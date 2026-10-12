#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>

#include <vector>

namespace Desert::Editor::Render
{
    // A tool's temporary mesh in a scene's view - UE's UPreviewMesh, which UAddPrimitiveTool shows where the
    // click will place the shape: drawn BY THE SCENE RENDER with its depth (half hidden behind a wall when it
    // stands half behind it), translucent, and no entity - nothing in the scene, its undo or its file.
    struct ToolPreviewMesh
    {
        // World space, three vertices per triangle, already ordered far to near (the translucent faces
        // blend in the order a depth write would have kept); the vertex colour carries the shading.
        std::vector<Graphic::MaterialDebugLine::LineVertex> Triangles;
        // World space, two vertices per edge: the form's edges, depth-tested like the faces.
        std::vector<Graphic::MaterialDebugLine::LineVertex> Edges;
        // One per edge: the unit world normal of the surface the edge lies on (its most grazing face). The pass
        // lifts each edge off that surface by what the surface's depth changes across a pixel - the slope-scaled
        // depth bias a rasterizer gives polygons and never gives line primitives.
        std::vector<glm::vec3> EdgeSurfaces;
    };

    // The previews a tool shows, one per scene. A tool shows its preview every frame it is active and hides
    // it when it stops; a preview not shown again by the next frame is not drawn (the tool that showed it
    // is no longer updated - its panel closed, the editor switched mode), so it cannot outlive its tool.
    class ToolPreview
    {
    public:
        static void Show( const ::Desert::Core::Scene& scene, ToolPreviewMesh mesh );
        static void Hide( const ::Desert::Core::Scene& scene );
        // This frame's preview of `scene`, or nothing.
        [[nodiscard]] static const ToolPreviewMesh* Current( const ::Desert::Core::Scene& scene );
    };

    // Draws ToolPreview's mesh of its scene in the Debug phase into the scene HDR target, depth-tested against
    // the scene's geometry without writing depth: alpha-blended faces, then their edges. Hidden in Play mode.
    class EditorToolPreviewPass
    {
    public:
        ~EditorToolPreviewPass();

        // (Re)creates the two pipelines against the scene's CURRENT target and registers the pass. Call after
        // every Scene::Init.
        Common::BoolResultStr Install( const std::shared_ptr<::Desert::Core::Scene>& scene );

    private:
        std::weak_ptr<::Desert::Core::Scene>       m_Scene;
        std::shared_ptr<Graphic::GraphicsPipeline> m_FacePipeline;
        std::shared_ptr<Graphic::GraphicsPipeline> m_EdgePipeline;
        // One material per draw: each owns the storage buffer its draw reads.
        std::unique_ptr<Graphic::MaterialDebugLine> m_FaceMaterial;
        std::unique_ptr<Graphic::MaterialDebugLine> m_EdgeMaterial;
        // This frame's edges, lifted off their surfaces for this view (LiftEdges).
        std::vector<Graphic::MaterialDebugLine::LineVertex> m_LiftedEdges;
    };
} // namespace Desert::Editor::Render

#pragma once

#include <Common/Core/Math/Ray.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace Desert::Core
{
    class Scene;
} // namespace Desert::Core

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Deform -> Vertex Sculpt (UE UMeshVertexSculptTool): a brush on the selected static mesh's
    // vertices, its operators in Geometry/MeshSculptBrushes.hpp (Move, Smooth) with ModelingState::DeformSculpt.
    //   - hover: a ring of the brush radius on the surface under the cursor, in the plane across the hit normal;
    //   - stroke: the left button down on the mesh copies the tool target (GetToolTargetMesh) and every frame
    //     stamps the brush on the copy, which the entity draws (ECS::SetEditableMesh, no undo step). Move
    //     carries the vertices on UE's active plane: through the first hit, facing the cursor ray;
    //   - the stroke ends (release, another tool, another selection): the tangents are rebuilt and the stroke is
    //     ONE undo step (Commands::RecordMeshChange) from the mesh the component held before it. As in UE the
    //     component's mesh is the edit; writing it into the asset is the asset's own Accept / save.
    class VertexSculptTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool hovered );

        // The scene's left click belongs to the brush: a stroke is running, or the cursor is over the mesh.
        [[nodiscard]] bool OwnsClick() const { return m_Stroke.has_value() || m_HoverHit; }

    private:
        struct Stroke
        {
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Before; // what the component held: the undo "before"
            std::shared_ptr<Geometry::DynamicMesh3>       Working;
            glm::dvec3                                    PlanePoint{ 0.0 };  // world, Move's active plane
            glm::dvec3                                    PlaneNormal{ 0.0 }; // world
            glm::dvec3                                    PrevCenter{ 0.0 };  // object space
            int32_t                                       Moved = 0;
        };

        void EndStroke( ::Desert::Core::Scene& scene );

        std::optional<Stroke> m_Stroke;
        bool                  m_HoverHit = false;
        // The hit query over the mesh the brush is on; rebuilt when that mesh object or its contents change.
        std::shared_ptr<const Geometry::DynamicMesh3> m_TreeHold; // keeps the address the tree reads alive
        const Geometry::DynamicMesh3*  m_TreeMesh  = nullptr;
        uint64_t                       m_TreeStamp = 0;
        Geometry::DynamicMeshAABBTree3 m_Tree;
    };
} // namespace Desert::Editor::Tools

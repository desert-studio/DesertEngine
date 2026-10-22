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
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Attributes -> Paint PolyGroups (UE UMeshGroupPaintTool): the selected static mesh's
    // triangle groups painted with a brush. The tool owns a working copy of the tool target (GetToolTargetMesh):
    //   - Brush: the left button held on the mesh paints the current group into every triangle connected to the
    //     one under the cursor whose centroid lies within the brush radius (world centimetres);
    //   - Fill: a click floods the current group over the connected region of the clicked triangle's group;
    //   - New Group: the current group becomes an unused id (UE's "New Group" / AllocateTriangleGroup);
    //   - preview: every visible triangle is tinted with its group's colour and the group borders are drawn
    //     (UE shows the group colours on the preview mesh);
    //   - Accept records the painted mesh as ONE undo step (Commands::RecordMeshChange); Cancel, another tool or
    //     another entity drops the copy (the component was never changed).
    class PaintGroupsTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool hovered );

        // The scene's left click belongs to the brush while it is over the mesh or painting.
        [[nodiscard]] bool OwnsClick() const { return m_Painting || m_HoverHit; }

    private:
        struct Session
        {
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the undo "before"
            std::shared_ptr<Geometry::DynamicMesh3>       Working;   // the painted copy
            int32_t                                       Painted = 0; // triangles changed since Begin
        };

        // The component keeps its mesh until Accept (groups draw nothing of their own; the preview is the
        // overlay), so ending a session without Accept has nothing to put back.
        void Begin( ::Desert::Core::Scene& scene );
        bool Accept( ::Desert::Core::Scene& scene );
        // Paints the current group from @p seed (brush region or flood fill); returns the triangles changed.
        int  Paint( int seed, const glm::dvec3& centreObject, double radiusObject );
        void DrawPreview( const glm::dmat4& world, const glm::dvec3& eyeWorld, const glm::mat4& viewProj,
                          const glm::vec2& viewportPos, const glm::vec2& viewportSize ) const;

        std::optional<Session> m_Session;
        bool                   m_Painting = false;
        bool                   m_HoverHit = false;
        uint64_t               m_TreeStamp = 0;
        const Geometry::DynamicMesh3*  m_TreeMesh = nullptr;
        Geometry::DynamicMeshAABBTree3 m_Tree;
    };
} // namespace Desert::Editor::Tools

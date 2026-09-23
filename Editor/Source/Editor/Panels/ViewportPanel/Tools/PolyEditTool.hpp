#pragma once

#include <Common/Core/UUID.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Geometry
{
    class EditMesh;
}

namespace Desert::Editor::Tools
{
    // PolyEdit (Modeling / Model category), phase 1: click a FACE of the selected entity's mesh — the
    // coplanar polygroup under the cursor highlights green — then LMB-drag to push/pull it along its normal
    // (an EditMesh corner is one vertex, so shared edges follow). A full 3-axis gizmo + the rest of the
    // PolyGroup-Edit operations (Extrude, Bevel, Inset, …) come next. Edits the entity's EditMesh (the source of
    // truth, StaticMeshComponent::EditableMesh); a finished drag is one undo step.
    class PolyEditTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive );

    private:
        void        ClearSelection();
        void        FinishDrag();
        bool        PickFace( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray );
        static bool WorldToScreen( const glm::vec3& world, const glm::mat4& vp, const glm::vec2& pos,
                                   const glm::vec2& size, glm::vec2& out );

        Common::UUID     m_Entity = Common::UUID::Null(); // entity whose mesh we edit (the current selection)
        std::vector<int> m_SelVerts;                      // EditMesh vertex IDs moved together
        std::vector<int> m_SelTris;                       // EditMesh triangle IDs of the face (for the highlight)
        glm::vec3        m_FaceNormalLocal{ 0, 1, 0 };
        glm::vec3        m_CentroidWorld{ 0 };
        bool             m_HasSel   = false;
        bool             m_Dragging = false;
        float            m_DragS    = 0.0f; // last push parameter along the world normal line
        // The drag's undo record: whose mesh, and the mesh it started from (immutable, so kept by reference).
        Common::UUID                              m_DragEntity = Common::UUID::Null();
        std::shared_ptr<const Geometry::EditMesh> m_DragBefore;
    };
} // namespace Desert::Editor::Tools

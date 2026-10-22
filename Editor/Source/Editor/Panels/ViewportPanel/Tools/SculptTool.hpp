#pragma once

#include <Common/Core/UUID.hpp>
#include <Common/Core/Math/Ray.hpp>
#include <Editor/Panels/ViewportPanel/Tools/ToolCursor.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Geometry
{
    class DynamicMesh3;
    struct RemeshSettings;
}

namespace Desert::Editor::Tools
{
    // Remesh (Modeling / Deform palette; UE's URemeshMeshTool): the selected static mesh remeshed whole to
    // settings.TargetEdgeLength (Geometry/DynamicMeshRemesh.hpp) as ONE undo step on its editable mesh; Accept
    // writes it into the .stmesh. Returns why it refused, or nullopt when the mesh was remeshed.
    [[nodiscard]] std::optional<std::string> RemeshSelected( ::Desert::Core::Scene&          scene,
                                                             const Geometry::RemeshSettings& settings );

    // Sculpt (Modeling / Deform palette; UE's UMeshVertexSculptTool): LMB-drag across the selected static mesh
    // lays brush stamps along the stroke (Geometry/DynamicMeshSculpt.hpp), with the brush, size, strength and
    // falloff from ModelingState::Sculpt. Shift held = Smooth for the stroke, Ctrl held = Invert (UE's
    // modifiers). Edits the entity's DynamicMesh3 (StaticMeshComponent::EditableMesh); one stroke is one undo
    // step; "Accept: write the edit into the static mesh asset" saves it to the .stmesh.
    class SculptTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive );

        // Drag mid-stroke, Select over the mesh (a press starts a stroke), Unavailable off it or with no mesh.
        [[nodiscard]] ToolCursor Cursor() const
        {
            return m_Cursor;
        }

    private:
        void FinishStroke();

        Common::UUID m_Entity   = Common::UUID::Null();
        ToolCursor   m_Cursor   = ToolCursor::None;
        bool         m_Stroking = false;
        // Where the last stamp landed (world): the next one is laid a spacing further along the stroke.
        std::optional<glm::dvec3> m_LastStamp;
        // The stroke's undo record: whose mesh, and the mesh it started from (immutable, kept by reference).
        Common::UUID                                  m_StrokeEntity = Common::UUID::Null();
        std::shared_ptr<const Geometry::DynamicMesh3> m_StrokeBefore;
    };
} // namespace Desert::Editor::Tools

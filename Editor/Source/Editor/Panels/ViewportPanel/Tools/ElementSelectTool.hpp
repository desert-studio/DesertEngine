#pragma once

#include <Common/Core/Math/Ray.hpp>

#include <glm/glm.hpp>

#include <optional>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode's "Select Elements" tool: clicks pick vertices / edges / triangles / polygroups of the
    // selected entity's EditMesh into Core::MeshElementSelection and draws that selection over the mesh.
    //   LMB        - replace the selection with the element under the cursor (empty space clears it)
    //   Shift+LMB  - add it
    //   Ctrl+LMB   - remove it
    //   Alt+K      - the knife: the next two clicks draw a line, the selected polygroups are cut along it
    // Every click that changes the selection is one undo step. The element under the cursor is outlined
    // while hovering, so the pick tolerance is visible before the click.
    class ElementSelectTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive );

        // Screen-space pick radius for vertices and edges.
        static constexpr float kTolerancePixels = 8.0f;

    private:
        bool                     m_KnifeArmed = false; // Alt+K: the next two clicks draw the cut line
        std::optional<glm::vec2> m_KnifeStart;         // its first end, in screen pixels
    };
} // namespace Desert::Editor::Tools

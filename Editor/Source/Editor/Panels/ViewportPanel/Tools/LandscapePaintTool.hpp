#pragma once

#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Engine/Desert.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <optional>

namespace Desert::Editor::Tools
{
    /**
     * @brief Landscape mode's Paint tool in the viewport — UE's FLandscapeToolPaint driven the way
     * FEdModeLandscape drives it: a stroke starts on LMB press, applies once per frame at the landscape point
     * under the cursor (Shift erases, UE's bInvert), and ends on release as ONE undo transaction.
     *
     * The target layer and its blend rules come from the landscape root's LandscapeComponent::Layers, read when
     * the press begins; the brush is the sculpt tools' brush, as in UE.
     */
    class LandscapePaintTool
    {
    public:
        /// @p mouseRay under the cursor, @p centreRay through the viewport centre (palette strokes use it). No
        /// frame time: UE's paint stroke applies a fixed amount per application.
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                     const Common::Math::Ray& centreRay, bool hovered );

    private:
        Common::BoolResultStr Begin( ::Desert::Core::Scene& scene );
        Common::BoolResultStr Step( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool invert );
        void                  End( ::Desert::Core::Scene& scene );

        std::optional<ECS::LandscapeEditTarget>               m_Target;
        std::optional<World::Landscape::LandscapePaintStroke> m_Stroke;
        bool                                                  m_Failed = false;
    };
} // namespace Desert::Editor::Tools

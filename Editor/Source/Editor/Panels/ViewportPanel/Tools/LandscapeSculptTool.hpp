#pragma once

#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Engine/Desert.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <glm/glm.hpp>

#include <optional>

namespace Desert::Editor::Tools
{
    /**
     * @brief Landscape mode's Sculpt / Smooth tool in the viewport — UE's FLandscapeToolSculpt / Smooth driven the
     *        way FEdModeLandscape drives them: a stroke starts on LMB press, applies once per frame at the
     *        landscape point under the cursor (Shift lowers), and ends on release as ONE undo transaction.
     *
     * The ray is traced against the landscape alone (UE's LandscapeTrace), not Scene::Raycast: a mesh standing
     * on the terrain must not catch the brush.
     */
    class LandscapeSculptTool
    {
    public:
        /// @p mouseRay under the cursor, @p centreRay through the viewport centre (palette strokes use it).
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                     const Common::Math::Ray& centreRay, bool hovered, float deltaSeconds );

    private:
        Common::BoolResultStr Begin( ::Desert::Core::Scene& scene );
        Common::BoolResultStr Step( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool invert,
                                    float deltaSeconds );
        void                  End( ::Desert::Core::Scene& scene );
        void SetRampPoint( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool start );
        /// Serves a pending ramp request (a point, apply, reset); false when the request is not the ramp's.
        bool ServeRampRequest( ::Desert::Core::Scene& scene, const Common::Math::Ray& centreRay );
        /// Serves a Mirror or Copy/Paste request at @p ray (a point, a copy, a paste, the mirror itself).
        void ServeComponentRequest( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                    ::Desert::Editor::Core::LandscapeStrokeRequest request );

        std::optional<ECS::LandscapeEditTarget>                m_Target;
        std::optional<World::Landscape::LandscapeHeightStroke> m_Stroke;
        bool                                                   m_Failed = false;
        /// The undo entry's name, fixed when the stroke begins.
        const char* m_ToolName = "";
    };
} // namespace Desert::Editor::Tools

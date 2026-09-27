#pragma once

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/ECS/PostProcessVolumeComponent.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>

namespace Desert::Graphic
{
    // The directional light's shadow policy as the renderer consumes it (UE: the Cascaded Shadow Maps
    // section of UDirectionalLightComponent). The defaults are what a view renders with when the scene has
    // no directional light at all — the cascades are then empty either way.
    struct ViewShadowSettings
    {
        bool  Enabled            = true;
        float Bias               = 0.005f;
        float CascadeSplitLambda = 0.6f;
    };

    // EVERYTHING A VIEW IS GRADED AND SHADOWED WITH, RESOLVED IN ONE PLACE — UE's FFinalPostProcessSettings.
    // SceneRenderer::BeginScene calls ResolveViewSettings once per view and reads these values and nothing
    // else; no render pass reads a PostProcessVolume or a DirectionalLight's shadow fields directly. That
    // is what lets the passes be rewritten (RDG3) without touching where the values come from, and the
    // values' source change (SET1) without touching the passes.
    struct FinalViewSettings
    {
        Core::PostProcessSettings Post;
        ViewShadowSettings        Shadows;
    };

    // Weight a volume contributes at `viewPosition`: BlendWeight inside (or for an Unbound volume anywhere),
    // fading linearly to 0 across BlendRadius outside the box. No view position (a view without a camera)
    // reaches only Unbound volumes.
    [[nodiscard]] float PostProcessVolumeWeight( const ECS::PostProcessVolumeData& volume,
                                                 const glm::vec3&                  volumePosition,
                                                 const std::optional<glm::vec3>&   viewPosition );

    // Applies one volume onto `target` with weight `weight` (> 0): floats and colours are lerped, booleans,
    // enums and counts are taken whole — UE's LERP_PP and override-assign split.
    void BlendPostProcessSettings( Core::PostProcessSettings& target, const Core::PostProcessSettings& volume,
                                   float weight );

    // The one resolution point. Visible PostProcessVolume entities (VisibilityComponent honoured) are
    // applied in ascending Priority onto Core::PostProcessSettings{}; the shadow policy is the first visible,
    // non-degenerate directional light's — the same light Scene::OnUpdate elects to shade with.
    [[nodiscard]] FinalViewSettings ResolveViewSettings( const entt::registry&           registry,
                                                         const std::optional<glm::vec3>& viewPosition );
} // namespace Desert::Graphic

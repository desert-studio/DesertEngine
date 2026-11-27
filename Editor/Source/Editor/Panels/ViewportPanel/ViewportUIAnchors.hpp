#pragma once

#include <UI/UILayout.hpp>

#include <entt/entt.hpp>

namespace Desert::Editor
{
    // Unity/UE-style anchor presets. Each axis is Min/Center/Max (a fixed-size box pinned to that edge,
    // keeping the element's current size) or Stretch (anchors 0..1, zero offsets -> fills the parent on
    // that axis). Stretch+Stretch = "fill parent" (the full-quad the UI needs). Offsets are in the same
    // screen-px space UICanvasLayout/RenderCanvas2D resolve layout in.
    enum class AnchorAxis
    {
        Min,
        Center,
        Max,
        Stretch
    };

    /// Pins the UI element @p e to the preset (@p hx, @p vy) inside its canvas, keeping its design-space size.
    void ApplyAnchorPreset( entt::registry& reg, entt::entity e, const ::Desert::UI::Rect& viewRect, AnchorAxis hx,
                            AnchorAxis vy );
} // namespace Desert::Editor

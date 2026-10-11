#pragma once

// The engine side of UI/UICanvasRenderer2D.hpp and UI/UIOverlay.hpp: the `entt::registry&` overloads every engine
// and editor caller uses. Each wraps the registry in an EcsUITree and asks the IUITree overload of the framework
// (DesertUI), so there is one walk, not one per storage. The framework itself never names entt
// (Desert/Tests/Engine/UIFrameworkBoundary).
#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIOverlay.hpp>

#include <Engine/UI/Ecs/EcsUITree.hpp>

namespace Desert::UI
{
    void BeginUIFrame( UIViewContext& view, entt::registry& reg, const Rect& viewportPx, float frameDtSeconds );
    void EndUIFrame( UIViewContext& view, entt::registry& reg, Graphic::Render2D::DrawList2D& dl,
                     const UIInput* input, entt::entity* focused = nullptr, std::string* outClicked = nullptr,
                     std::vector<std::string>* outMessages = nullptr );
    NO_DISCARD Common::BoolResultStr RenderCanvas2D( UIViewContext& view, entt::registry& reg, entt::entity canvas,
                                                     Graphic::Render2D::DrawList2D& dl,
                                                     const glm::mat4*               worldViewProj = nullptr,
                                                     const UIInput*                 input         = nullptr,
                                                     std::string*                   outClicked    = nullptr,
                                                     entt::entity*                  focused       = nullptr );

    [[nodiscard]] Common::ResultStr<entt::entity> OverlayByName( entt::registry& reg, const std::string& name );
} // namespace Desert::UI

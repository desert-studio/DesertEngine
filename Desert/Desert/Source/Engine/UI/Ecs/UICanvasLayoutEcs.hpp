#pragma once

// The engine side of UI/UICanvasLayout.hpp: the `entt::registry&` overloads every engine and editor caller uses.
// Each wraps the registry in an EcsUITree and asks the IUITree overload of the framework (DesertUI), so there is
// one walk, not one per storage. The framework itself never names entt (Desert/Tests/Engine/UIFrameworkBoundary).
#include <UI/UICanvasLayout.hpp>

#include <Engine/UI/Ecs/EcsUITree.hpp>

namespace Desert::UI
{
    // --- ECS overloads (Engine/UI/Ecs/UICanvasLayoutEcs.cpp) -----------------------------------------
    // The same questions with the signatures every engine and editor caller already has: each wraps @p reg in
    // an EcsUITree and asks the tree overload above, so there is one walk, not one per storage. UIElementNode
    // ids are NodeIds either way; UI::ToEntity / UI::ToNode (Ecs/EcsUITree.hpp) convert, bit for bit.
    [[nodiscard]] entt::entity                    CanvasOf( entt::registry& reg, entt::entity e );
    [[nodiscard]] std::size_t                     CanvasCount( entt::registry& reg );
    [[nodiscard]] Common::ResultStr<entt::entity> SoleCanvas( entt::registry& reg );
    [[nodiscard]] std::vector<entt::entity>       CanvasesInDrawOrder( entt::registry& reg );
    [[nodiscard]] bool                            TakesLayoutSpace( entt::registry& reg, entt::entity e );
    [[nodiscard]] bool                            IsElementVisible( entt::registry& reg, entt::entity e );
    NO_DISCARD Common::BoolResultStr EnumerateCanvas( entt::registry& reg, entt::entity canvas,
                                                      const Rect& viewportPx, std::vector<UIElementNode>& out,
                                                      const struct UICanvasContext* ctx = nullptr );
    [[nodiscard]] bool               BindingHidesElement( entt::registry& reg, entt::entity e,
                                                          const struct UICanvasContext* ctx = nullptr,
                                                          const UIDataStore*            row = nullptr );
    [[nodiscard]] entt::entity PickElement( entt::registry& reg, entt::entity canvas, const glm::vec2& pointPx,
                                            const Rect& viewportPx );
    [[nodiscard]] bool         GetElementRect( entt::registry& reg, entt::entity canvas, entt::entity target,
                                               const Rect& viewportPx, Rect& out, glm::mat3* outXform = nullptr );
    [[nodiscard]] Common::ResultStr<float> CanvasScale( entt::registry& reg, entt::entity canvas,
                                                        const Rect& viewportPx );
} // namespace Desert::UI

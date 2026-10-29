#pragma once

#include <Editor/Core/IPanel.hpp>

#include <Common/Settings/Scalability.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace Desert::Editor
{
    // Machine quality (UE's Scalability settings): an overall level and one level per quality group, then — folded
    // away under "Advanced" — the per-parameter overrides. Everything goes through
    // Common::Scalability::QualityState.
    class ScalabilityPanel final : public IPanel
    {
    public:
        explicit ScalabilityPanel( const std::shared_ptr<Desert::Core::Scene>& scene );

        void OnUIRender() override;

        // The active scene, read for its render path: MSAA is offered only on the forward path (AA2).
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        // The Basic parameters (ParameterVisibility::Basic) with their curated controls.
        void DrawBasicParameters();
        // Every listed Advanced parameter, one closed tree node per group, drawn from its spec.
        void DrawInternalParameters();
        // One spec-driven row: a combo over the value names, else a slider over [Min, Max]; Reset when overridden.
        void DrawParameterRow( Common::Scalability::Parameter parameter );

        std::weak_ptr<Desert::Core::Scene> m_Scene;
        // The render-scale slider's value while it is dragged; applied (and saved) once, on release.
        std::optional<int> m_DraggedRenderScale;
        // The same for the one spec-driven slider being dragged.
        std::optional<std::pair<Common::Scalability::Parameter, int>> m_DraggedParameter;
    };
} // namespace Desert::Editor

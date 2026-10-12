#pragma once

#include "../IPanel.hpp"

#include <memory>
#include <optional>

namespace Desert::Editor
{
    // Machine quality (AA, MSAA, render scale, texture filtering, cloud tier) — Common::Settings::MachineSettings.
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
        std::weak_ptr<Desert::Core::Scene> m_Scene;
        // The render-scale slider's value while it is dragged; applied (and saved) once, on release.
        std::optional<int> m_DraggedRenderScale;
    };
} // namespace Desert::Editor

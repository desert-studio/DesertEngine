#pragma once

#include "../IPanel.hpp"

#include <memory>

namespace Desert::Editor
{
    // Machine quality (AA, MSAA, texture filtering, cloud tier) — Common::Settings::MachineSettings.
    class ScalabilityPanel final : public IPanel
    {
    public:
        ScalabilityPanel();

        void OnUIRender() override;

        // The active scene, read for its render path: MSAA is offered only on the forward path (AA2).
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        std::weak_ptr<Desert::Core::Scene> m_Scene;
    };
} // namespace Desert::Editor

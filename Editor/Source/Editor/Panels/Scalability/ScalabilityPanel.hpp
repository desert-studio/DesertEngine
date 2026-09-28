#pragma once

#include "../IPanel.hpp"

namespace Desert::Editor
{
    // Machine quality (AA, MSAA, texture filtering, cloud tier) — Common::Settings::MachineSettings.
    class ScalabilityPanel final : public IPanel
    {
    public:
        ScalabilityPanel();

        void OnUIRender() override;
    };
} // namespace Desert::Editor

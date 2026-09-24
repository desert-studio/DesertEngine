#pragma once

#include "../IPanel.hpp"

#include <memory>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // UE5-style Modeling Mode palette: a left category rail (Create / Select / XForm / …), a 2-column tool
    // grid, and a Tool Properties section for the active tool. Currently the Create category's CubeGrid tool
    // is functional (the rest are placeholders). Selecting CubeGrid switches the viewport into Modeling mode;
    // the actual geometry editing happens in the viewport (CubeGridTool), driven through Core::ModelingState.
    class ModelingPanel final : public IPanel
    {
    public:
        explicit ModelingPanel( const std::shared_ptr<Desert::Core::Scene>& scene );

        void OnUIRender() override;

        // Contextual: the viewport is in Modeling mode.
        bool IsContextual() const override
        {
            return true;
        }

        bool IsRelevant() const override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        // The Select Elements tool's properties: mode, counts, and the selection operations.
        void DrawCreateShape();
        static void DrawOutputType();
        void DrawElementSelection();

        std::shared_ptr<Desert::Core::Scene> m_Scene;
        int                                  m_Category = 0; // index into the rail: 0 = Create, 1 = Model
    };
} // namespace Desert::Editor

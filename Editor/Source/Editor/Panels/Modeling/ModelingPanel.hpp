#pragma once

#include "../IPanel.hpp"

#include <Common/Core/ResultStr.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/MeshXformOperations.hpp>
#include <Common/Core/UUID.hpp>

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
        // One per rail entry, in UE's palette order (the Palette enum in ModelingPanel.cpp).
        void DrawSelectionPalette();
        void DrawShapesPalette();
        void DrawCreatePalette();
        void DrawPolyModelPalette();
        void DrawTriModelPalette();
        void DrawTransformPalette();
        void DrawCubeGrid();
        // A mesh / XForm operation on one click; a refusal is logged with its reason.
        void Operate( Core::MeshOperation op );
        void Transform( Core::XformOperation op );
        void RunBoolean( Core::BooleanTool tool );
        // Boolean and Trim share their Output options (UE's Write To and Handle Inputs).
        static void DrawBooleanOutput();

        std::shared_ptr<Desert::Core::Scene> m_Scene;
        int                                  m_Category  = 0; // index into the rail (Palette in the .cpp)
        int                                  m_ShownTool = 0; // the ModelingState::Tool the rail last followed
    };
} // namespace Desert::Editor

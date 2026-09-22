#pragma once

#include "Editor/Panels/IPanel.hpp"

#include <ImGui/imgui.h> // the panel interface no longer hands the toolkit over (IPanel.hpp)
#include <Engine/Desert.hpp>

namespace Desert::Editor
{
    class ShaderLibraryPanel : public IPanel
    {
    public:
        ShaderLibraryPanel();
        void OnUIRender() override;
        void RenderShaderCodeWindow();

    private:
        void DrawShaderNode( const std::string& name, const std::shared_ptr<Graphic::Shader>& shader );
        void SaveShaderCode();

    private:
        Graphic::Shader* m_SelectedShader;
        std::string      m_ShaderSource;
        bool             m_ShowShaderCode = false;

        bool            m_EditMode = false;
        ImGuiTextFilter m_ShaderFilter;
    };
} // namespace Desert::Editor
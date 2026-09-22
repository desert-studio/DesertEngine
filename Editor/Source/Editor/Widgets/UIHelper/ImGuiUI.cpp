#include "ImGuiUI.hpp"

#include <Engine/Desert.hpp>

namespace Desert::Editor::UI
{
    namespace ImGui = ::ImGui;

    void UIHelper::Init()
    {
        m_CacherTexture = std::make_unique<UICacheTextureImGui>();
    }

    const void* UIHelper::GetTextureID( const std::shared_ptr<Graphic::Image2D>& image )
    {
        return ( m_CacherTexture && image ) ? m_CacherTexture->AddTextureCache( image ) : nullptr;
    }

    void UIHelper::Image( const std::shared_ptr<Graphic::Image2D>& image, const ImVec2& size, const ImVec2& uv0,
                          const ImVec2& uv1, const ImVec4& tint_col, const ImVec4& border_col )
    {
        if ( !image || size.x <= 0.0f || size.y <= 0.0f )
            return;

        const auto* id = m_CacherTexture->AddTextureCache( image );

        ::ImGui::Image( (ImTextureID)id, size, uv0, uv1, tint_col, border_col );
    }

    bool UIHelper::ImageButton( const char* strId, const std::shared_ptr<Graphic::Image2D>& image,
                                const ImVec2& size )
    {
        if ( !image || size.x <= 0.0f || size.y <= 0.0f )
            return false;

        const auto* id = m_CacherTexture->AddTextureCache( image );

        // Borderless thumbnail button (no frame padding, transparent bg) that's still a real, draggable item.
        ::ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        const bool clicked = ::ImGui::ImageButton( strId, (ImTextureID)id, size, ImVec2( 0, 0 ),
                                                   ImVec2( 1, 1 ), ImVec4( 0, 0, 0, 0 ) );
        ::ImGui::PopStyleVar();
        return clicked;
    }
} // namespace Desert::Editor::UI
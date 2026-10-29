#include "Mockup.hpp"

#include <Editor/Core/ThemeManager.hpp>

#include <array>

// Every mockup the tool knows. A new mockup is one file under Mockups/ plus one declaration and one row here.
namespace Desert::UIMockup::Mockups
{
    void VfxSystemWindow( const Fonts& fonts );
    void VfxPaintMode( const Fonts& fonts );
} // namespace Desert::UIMockup::Mockups

namespace Desert::UIMockup
{
    std::span<const Mockup> AllMockups()
    {
        static const std::array<Mockup, 2> kMockups = { {
             { "vfx-system",
               "VFX System document window (.dfx): toolbar, emitters, preview, module stack, "
               "timeline and curves",
               &Mockups::VfxSystemWindow },
             { "vfx-paint", "VFX Paint mode in the level viewport: brush tools that author emission shapes",
               &Mockups::VfxPaintMode },
        } };
        return kMockups;
    }

    const Mockup* FindMockup( std::string_view name )
    {
        for ( const Mockup& mockup : AllMockups() )
            if ( mockup.name == name )
                return &mockup;
        return nullptr;
    }

    void SectionHeader( const Fonts& fonts, const char* label, const char* trailing )
    {
        ImDrawList*  draw   = ImGui::GetWindowDrawList();
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        const float  height = ImGui::GetFrameHeight() + 2.0f;
        draw->AddRectFilled( start, start + ImVec2( width, height ),
                             ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetSectionHeaderColor() ) );

        ImGui::PushFont( fonts.bold );
        const float textY = start.y + ( height - ImGui::GetTextLineHeight() ) * 0.5f;
        draw->AddText( ImVec2( start.x + 8.0f, textY ), ImGui::GetColorU32( ImGuiCol_Text ), label );
        ImGui::PopFont();
        if ( trailing != nullptr )
        {
            const float trailingWidth = ImGui::CalcTextSize( trailing ).x;
            draw->AddText( ImVec2( start.x + width - trailingWidth - 8.0f,
                                   start.y + ( height - ImGui::GetTextLineHeight() ) * 0.5f ),
                           ImGui::GetColorU32( ImGuiCol_TextDisabled ), trailing );
        }
        ImGui::Dummy( ImVec2( width, height ) );
        ImGui::Spacing();
    }

    ImRect ViewportPlaceholder( const char* caption, ImVec2 size )
    {
        ImDrawList*  draw  = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImRect rect( start, start + size );

        draw->AddRectFilledMultiColor( rect.Min, rect.Max, Rgb( 46, 52, 62 ), Rgb( 46, 52, 62 ), Rgb( 24, 24, 26 ),
                                       Rgb( 24, 24, 26 ) );

        // A floor grid in one-point perspective, so the rectangle reads as "a 3D view goes here".
        draw->PushClipRect( rect.Min, rect.Max, true );
        const ImVec2 horizon( rect.GetCenter().x, rect.Min.y + rect.GetHeight() * 0.42f );
        for ( int i = -14; i <= 14; ++i )
        {
            const float x = rect.GetCenter().x + static_cast<float>( i ) * rect.GetWidth() * 0.12f;
            draw->AddLine( horizon, ImVec2( x, rect.Max.y ), Rgb( 70, 70, 74, 120 ) );
        }
        for ( int row = 1; row <= 10; ++row )
        {
            const float t = static_cast<float>( row * row ) / 100.0f;
            const float y = horizon.y + ( rect.Max.y - horizon.y ) * t;
            draw->AddLine( ImVec2( rect.Min.x, y ), ImVec2( rect.Max.x, y ), Rgb( 70, 70, 74, 120 ) );
        }
        draw->PopClipRect();

        const ImVec2 captionSize = ImGui::CalcTextSize( caption );
        draw->AddText( ImVec2( rect.GetCenter().x - captionSize.x * 0.5f, rect.Min.y + 12.0f ),
                       ImGui::GetColorU32( ImGuiCol_TextDisabled ), caption );
        draw->AddRect( rect.Min, rect.Max, ImGui::GetColorU32( ImGuiCol_Border ) );

        ImGui::Dummy( size );
        return rect;
    }

    bool ToolButton( const char* label, bool on, ImVec2 size )
    {
        if ( on )
        {
            const ImVec4 amber = Editor::ThemeManager::GetHighlightColor();
            ImGui::PushStyleColor( ImGuiCol_Button,
                                   ImVec4( amber.x * 0.45f, amber.y * 0.45f, amber.z * 0.45f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, amber );
        }
        const bool pressed = ImGui::Button( label, size );
        if ( on )
            ImGui::PopStyleColor( 2 );
        return pressed;
    }
} // namespace Desert::UIMockup

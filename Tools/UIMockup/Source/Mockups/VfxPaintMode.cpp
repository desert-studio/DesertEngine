// MOCKUP: the "VFX Paint" editor mode in the level viewport (VFX port plan §4.2).
//
// Three authoring tools write an emission shape into the selected VFXComponent instance - Paint Emission
// Area (a brush, ported from the Foliage brush, writes a serialised PointSet the "Shape: Painted Set" module
// reads through a PointSet DI), Spline Emission and Mesh Surface (vertex-colour mask) - plus Place & Tweak.
// Left: the mode panel; right: the level viewport (a placeholder here) with the brush and painted points.

#include "Mockup.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace Desert::UIMockup::Mockups
{
    namespace
    {
        enum class Tool
        {
            PaintArea,
            Spline,
            MeshSurface,
            PlaceTweak
        };

        // One labelled row: dim label in a fixed column, the widget filling the rest.
        template <typename WidgetFn>
        void Row( const char* label, WidgetFn&& widget )
        {
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 8.0f );
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( label );
            ImGui::SameLine( 150.0f );
            ImGui::SetNextItemWidth( -8.0f );
            widget();
        }

        void ModeStrip()
        {
            static constexpr std::array<const char*, 6> kModes = {
                 ICON_MDI_CURSOR_MOVE " Select",    ICON_MDI_GRID " Landscape", ICON_MDI_FORMAT_PAINT " Foliage",
                 ICON_MDI_CUBE_OUTLINE " Modeling", ICON_MDI_FIRE " VFX Paint", ICON_MDI_LAYERS " Fracture" };
            ImGui::SetCursorPos( ImGui::GetCursorPos() + ImVec2( 8.0f, 4.0f ) );
            for ( size_t i = 0; i < kModes.size(); ++i )
            {
                if ( i > 0 )
                    ImGui::SameLine( 0, 2 );
                ToolButton( kModes[i], i == 4 );
            }
            ImGui::SameLine( 0, 24 );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled( "Level: Factory_District.desce" );
        }

        void ToolGrid( Tool& tool )
        {
            struct Entry
            {
                Tool        tool;
                const char* icon;
                const char* label;
            };
            static constexpr std::array<Entry, 4> kTools = { {
                 { Tool::PaintArea, ICON_MDI_SPRAY, "Paint Area" },
                 { Tool::Spline, ICON_MDI_VECTOR_CURVE, "Spline" },
                 { Tool::MeshSurface, ICON_MDI_TEXTURE_BOX, "Mesh Surface" },
                 { Tool::PlaceTweak, ICON_MDI_CURSOR_MOVE, "Place & Tweak" },
            } };
            const float width = ( ImGui::GetContentRegionAvail().x - 16.0f - 3.0f * 4.0f ) / 4.0f;
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 8.0f );
            for ( size_t i = 0; i < kTools.size(); ++i )
            {
                if ( i > 0 )
                    ImGui::SameLine( 0, 4 );
                char label[64];
                std::snprintf( label, sizeof label, "%s\n%s", kTools[i].icon, kTools[i].label );
                ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.5f, 0.5f ) );
                if ( ToolButton( label, tool == kTools[i].tool, ImVec2( width, 54.0f ) ) )
                    tool = kTools[i].tool;
                ImGui::PopStyleVar();
            }
            ImGui::Spacing();
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 8.0f );
            ImGui::PushTextWrapPos( ImGui::GetContentRegionMax().x - 8.0f );
            ImGui::TextDisabled( "Brush on scene surfaces writes points (position, normal, weight) into the "
                                 "selected instance. The \"Shape: Painted Set\" module emits from them." );
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }

        void ModePanel( const Fonts& fonts )
        {
            static Tool  tool        = Tool::PaintArea;
            static float radius      = 120.0f;
            static float strength    = 0.6f;
            static float falloff     = 0.5f;
            static float density     = 40.0f;
            static bool  alignNormal = true;
            static bool  landscape   = true;
            static bool  staticMesh  = true;
            static bool  skeletal    = false;
            static float slope[2]    = { 0.0f, 75.0f };
            static int   weightFrom  = 0;

            SectionHeader( fonts, "VFX PAINT", "Shift erase · Ctrl+wheel radius" );
            ToolGrid( tool );

            SectionHeader( fonts, "TARGET" );
            Row( "Component",
                 [] { ImGui::Button( ICON_MDI_FIRE " SmokeStack_03  (VFXComponent)", ImVec2( -8, 0 ) ); } );
            Row( "System", [] { ImGui::TextUnformatted( "Content/VFX/ChimneySmoke.dfx" ); } );
            Row( "Point set",
                 []
                 {
                     ImGui::TextUnformatted( "PaintedPoints" );
                     ImGui::SameLine();
                     ImGui::TextDisabled( "1 284 points · 32 KB" );
                 } );
            Row( "Feeds", []
                 { ImGui::TextColored( ImVec4( 0.43f, 0.63f, 0.9f, 1.0f ), "Smoke  ›  Shape: Painted Set" ); } );
            ImGui::Spacing();

            SectionHeader( fonts, "BRUSH" );
            Row( "Radius", [] { ImGui::DragFloat( "##radius", &radius, 1.0f, 1.0f, 2000.0f, "%.0f cm" ); } );
            Row( "Strength", [] { ImGui::SliderFloat( "##strength", &strength, 0.0f, 1.0f, "%.2f" ); } );
            Row( "Falloff", [] { ImGui::SliderFloat( "##falloff", &falloff, 0.0f, 1.0f, "%.2f" ); } );
            Row( "Density", [] { ImGui::DragFloat( "##density", &density, 0.5f, 0.0f, 1000.0f, "%.0f / m²" ); } );
            Row( "Point weight",
                 []
                 {
                     static constexpr const char* kWeights[] = { "Brush strength", "Constant 1", "Falloff only" };
                     ImGui::Combo( "##weight", &weightFrom, kWeights, 3 );
                 } );
            Row( "Align to normal", [] { ImGui::Checkbox( "##align", &alignNormal ); } );
            ImGui::Spacing();

            SectionHeader( fonts, "FILTER" );
            Row( "Surfaces",
                 []
                 {
                     ImGui::Checkbox( "Landscape", &landscape );
                     ImGui::SameLine();
                     ImGui::Checkbox( "Static Mesh", &staticMesh );
                     ImGui::SameLine();
                     ImGui::Checkbox( "Skeletal", &skeletal );
                 } );
            Row( "Slope", [] { ImGui::DragFloat2( "##slope", slope, 0.5f, 0.0f, 90.0f, "%.0f°" ); } );
            Row( "Material", [] { ImGui::Button( "Any  " ICON_MDI_CHEVRON_DOWN, ImVec2( -8, 0 ) ); } );
            ImGui::Spacing();

            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + 8.0f );
            ImGui::Button( ICON_MDI_ERASER " Clear points" );
            ImGui::SameLine();
            ImGui::Button( ICON_MDI_SELECTION_DRAG " Select points" );
            ImGui::SameLine();
            ImGui::Button( ICON_MDI_UNDO " Undo stroke" );
        }

        void ViewportPanel()
        {
            const ImVec2 size = ImGui::GetContentRegionAvail();
            const ImRect rect = ViewportPlaceholder( "LEVEL VIEWPORT - the scene renders here", size );
            ImDrawList*  draw = ImGui::GetWindowDrawList();

            // A building facade in the placeholder: the surface being painted.
            const ImVec2 wallMin = rect.Min + ImVec2( rect.GetWidth() * 0.18f, rect.GetHeight() * 0.16f );
            const ImVec2 wallMax = rect.Min + ImVec2( rect.GetWidth() * 0.78f, rect.GetHeight() * 0.74f );
            draw->AddRectFilled( wallMin, wallMax, Rgb( 78, 74, 70 ) );
            draw->AddRect( wallMin, wallMax, Rgb( 40, 38, 36 ), 0.0f, 0, 2.0f );
            for ( int row = 0; row < 3; ++row )
                for ( int col = 0; col < 5; ++col )
                {
                    const ImVec2 w = wallMin + ImVec2( 40.0f + col * ( wallMax.x - wallMin.x - 80.0f ) / 4.6f,
                                                       40.0f + row * ( wallMax.y - wallMin.y - 60.0f ) / 3.0f );
                    draw->AddRectFilled( w, w + ImVec2( 54, 70 ), Rgb( 30, 34, 40 ) );
                }

            // Points already painted: clustered along the window tops, the way smoke would leak.
            unsigned state = 11u;
            auto     next  = [&state]()
            {
                state = state * 1664525u + 1013904223u;
                return static_cast<float>( ( state >> 8 ) & 0xFFFF ) / 65535.0f;
            };
            const ImU32 point = Rgb( 120, 220, 255, 220 );
            for ( int col = 1; col < 4; ++col )
            {
                const ImVec2 base =
                     wallMin + ImVec2( 40.0f + col * ( wallMax.x - wallMin.x - 80.0f ) / 4.6f + 27.0f,
                                       40.0f + ( wallMax.y - wallMin.y - 60.0f ) / 3.0f );
                for ( int i = 0; i < 46; ++i )
                {
                    const float  angle = next() * 6.2831853f;
                    const float  r     = 34.0f * std::sqrt( next() );
                    const ImVec2 p     = base + ImVec2( std::cos( angle ) * r, std::sin( angle ) * r * 0.6f );
                    draw->AddCircleFilled( p, 2.2f, point );
                    draw->AddLine( p, p + ImVec2( 0.0f, -5.0f ), Rgb( 120, 220, 255, 90 ) ); // normal
                }
            }

            // The brush under the cursor: outer radius, inner falloff ring, centre normal.
            const ImVec2 brush =
                 wallMin + ImVec2( ( wallMax.x - wallMin.x ) * 0.72f, ( wallMax.y - wallMin.y ) * 0.36f );
            const ImU32 amber = ImGui::ColorConvertFloat4ToU32( Editor::ThemeManager::GetHighlightColor() );
            draw->AddCircleFilled( brush, 62.0f, Rgb( 255, 190, 60, 28 ), 64 );
            draw->AddCircle( brush, 62.0f, amber, 64, 2.0f );
            draw->AddCircle( brush, 31.0f, Rgb( 255, 190, 60, 120 ), 48, 1.0f );
            draw->AddLine( brush, brush + ImVec2( 0.0f, -34.0f ), amber, 2.0f );
            for ( int i = 0; i < 22; ++i )
            {
                const float angle = next() * 6.2831853f;
                const float r     = 55.0f * std::sqrt( next() );
                draw->AddCircleFilled( brush + ImVec2( std::cos( angle ) * r, std::sin( angle ) * r ), 2.2f,
                                       Rgb( 255, 210, 120, 230 ) );
            }

            // Viewport overlays: view menu top-left, stroke stats top-right, key hints bottom.
            const ImU32  text = ImGui::GetColorU32( ImGuiCol_Text );
            const ImU32  dim  = ImGui::GetColorU32( ImGuiCol_TextDisabled );
            const ImVec2 menu = rect.Min + ImVec2( 10.0f, 10.0f );
            draw->AddRectFilled( menu, menu + ImVec2( 300.0f, 26.0f ), Rgb( 15, 15, 15, 200 ), 3.0f );
            draw->AddText( menu + ImVec2( 8.0f, 4.0f ), text,
                           ICON_MDI_CAMERA_CONTROL " Perspective   Lit   Show" );

            const ImVec2 stats = ImVec2( rect.Max.x - 270.0f, rect.Min.y + 10.0f );
            draw->AddRectFilled( stats, stats + ImVec2( 260.0f, 48.0f ), Rgb( 15, 15, 15, 200 ), 3.0f );
            draw->AddText( stats + ImVec2( 8.0f, 4.0f ), text, "Points 1 284   this stroke +22" );
            draw->AddText( stats + ImVec2( 8.0f, 24.0f ), dim, "Radius 120 cm   Density 40 / m²" );

            const char* hints =
                 "LMB paint   Shift+LMB erase   Ctrl+wheel radius   Alt+wheel strength   Esc leave mode";
            const ImVec2 hintSize = ImGui::CalcTextSize( hints );
            const ImVec2 hintAt( rect.GetCenter().x - hintSize.x * 0.5f, rect.Max.y - 34.0f );
            draw->AddRectFilled( hintAt - ImVec2( 10, 4 ), hintAt + hintSize + ImVec2( 10, 4 ),
                                 Rgb( 15, 15, 15, 200 ), 3.0f );
            draw->AddText( hintAt, dim, hints );
        }
    } // namespace

    void VfxPaintMode( const Fonts& fonts )
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( viewport->WorkPos );
        ImGui::SetNextWindowSize( viewport->WorkSize );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        ImGui::Begin( "##VfxPaintMode", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus );
        ImGui::PopStyleVar( 2 );

        const ImVec2 full = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled( ImGui::GetCursorScreenPos(),
                                                   ImGui::GetCursorScreenPos() + ImVec2( full.x, 40.0f ),
                                                   ImGui::GetColorU32( ImGuiCol_TitleBg ) );
        ImGui::BeginChild( "##modes", ImVec2( full.x, 40.0f ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground );
        ModeStrip();
        ImGui::EndChild();

        constexpr float kPanelWidth = 440.0f;
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 4, 4 ) );
        ImGui::BeginChild( "##panel", ImVec2( kPanelWidth, 0 ), true );
        ModePanel( fonts );
        ImGui::EndChild();
        ImGui::SameLine( 0, 2 );
        ImGui::BeginChild( "##viewport", ImVec2( 0, 0 ), false, ImGuiWindowFlags_NoScrollbar );
        ViewportPanel();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::End();
    }
} // namespace Desert::UIMockup::Mockups

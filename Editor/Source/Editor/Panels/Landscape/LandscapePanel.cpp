// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/SLandscapeEditor.cpp:537-575 and
// LandscapeEditorDetailCustomization_ToolStrip / _Brushes, adapted: Slate's mode toolbar, tool strip and property
// grid become ImGui buttons and the editor's property rows; UObject meta (ShowForTools, UIMin/UIMax, ClampMin/
// ClampMax) comes from LandscapeToolProperties(); the tool order is LandscapeEdMode.cpp:216-269's Sculpt mode.
//
// NOT PORTED, each for a stated reason. Manage and Paint mode tabs and the Target Layers section: this landscape
// has no weightmap layers (painting is LS-14) and no manage tools (new / resize / components), and an empty tab
// is a button that opens onto nothing (owner's decision, same as the Modeling rail). Alpha / Pattern / Component
// brush sets: the stroke maths has only UE's circle brush, so the brush row shows the one set that exists.

#include "LandscapePanel.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace Desert::Editor
{
    namespace
    {
        using Utils::ImGuiUtilities;

        struct ToolButton
        {
            Core::LandscapeTool Tool;
            const char*         Icon;
        };

        // UE's Sculpt mode strip, in its order (LandscapeEdMode.cpp:231-244); Visibility and Mask are absent
        // because this landscape has no visibility layer and no selection mask to paint.
        constexpr std::array<ToolButton, 10> kSculptTools = { {
             { Core::LandscapeTool::Sculpt, ICON_MDI_BRUSH },
             { Core::LandscapeTool::Erase, ICON_MDI_ERASER },
             { Core::LandscapeTool::Smooth, ICON_MDI_BLUR },
             { Core::LandscapeTool::Flatten, ICON_MDI_ARROW_COLLAPSE_VERTICAL },
             { Core::LandscapeTool::Ramp, ICON_MDI_SLOPE_UPHILL },
             { Core::LandscapeTool::Noise, ICON_MDI_GRAIN },
             { Core::LandscapeTool::Erosion, ICON_MDI_LANDSLIDE },
             { Core::LandscapeTool::HydroErosion, ICON_MDI_WEATHER_POURING },
             { Core::LandscapeTool::CopyPaste, ICON_MDI_CONTENT_COPY },
             { Core::LandscapeTool::Mirror, ICON_MDI_FLIP_HORIZONTAL },
        } };

        /// A button lit with the editor's one accent colour when @p active (UE's checked tool-strip entry).
        bool AccentButton( const char* label, bool active, const ImVec2& size )
        {
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            const bool pressed = ImGui::Button( label, size );
            if ( active )
                ImGui::PopStyleColor();
            return pressed;
        }

        void DrawProperty( const Core::LandscapeToolProperty& p, Core::LandscapeSculptSettings& s )
        {
            using Kind = Core::LandscapeToolProperty::Kind;
            ImGuiUtilities::BeginPropertyRow( p.Label );
            ImGui::SetNextItemWidth( -FLT_MIN );
            switch ( p.Type )
            {
                case Kind::Float:
                {
                    float*                 value = p.Float( s );
                    const ImGuiSliderFlags flags =
                         p.Logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
                    // The slider travels UE's UIMin..UIMax; Ctrl+click typing may go to ClampMin..ClampMax.
                    if ( ImGui::SliderFloat( "##v", value, p.UIMin, p.UIMax, p.UIMax < 2.0f ? "%.4g" : "%.1f",
                                             flags ) )
                        *value = std::clamp( *value, p.ClampMin, p.ClampMax );
                    break;
                }
                case Kind::Int:
                {
                    int32_t* value = p.Int( s );
                    int      v     = *value;
                    if ( ImGui::SliderInt( "##v", &v, static_cast<int>( p.UIMin ), static_cast<int>( p.UIMax ) ) )
                        *value = std::clamp( v, static_cast<int>( p.ClampMin ), static_cast<int>( p.ClampMax ) );
                    break;
                }
                case Kind::Bool:
                    ImGui::Checkbox( "##v", p.Bool( s ) );
                    break;
                case Kind::Choice:
                {
                    const int chosen = p.Chosen( s );
                    if ( ImGui::BeginCombo( "##v", p.Options[static_cast<size_t>( chosen )].c_str() ) )
                    {
                        for ( int i = 0; i < static_cast<int>( p.Options.size() ); ++i )
                            if ( ImGui::Selectable( p.Options[static_cast<size_t>( i )].c_str(), i == chosen ) )
                                p.Choose( s, i );
                        ImGui::EndCombo();
                    }
                    break;
                }
            }
            ImGuiUtilities::EndPropertyRow();
        }

        void PointRow( const char* label, const std::optional<glm::vec3>& point, const char* unset )
        {
            ImGuiUtilities::BeginPropertyRow( label );
            if ( point )
                ImGui::Text( "%.0f, %.0f, %.0f cm", point->x, point->y, point->z );
            else
                ImGui::TextDisabled( "%s", unset );
            ImGuiUtilities::EndPropertyRow();
        }
    } // namespace

    LandscapePanel::LandscapePanel() : IPanel( "Landscape", /*showPanel=*/false ) // contextual: the mode opens it
    {
    }

    bool LandscapePanel::IsRelevant() const
    {
        return Core::ViewportMode::Get() == Core::EditorMode::Landscape;
    }

    void LandscapePanel::OnUIRender()
    {
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 5.0f ) );

        // UE's mode row (Manage / Sculpt / Paint): only the mode with tools behind it is offered.
        AccentButton( ICON_MDI_TERRAIN "  Sculpt", true, ImVec2( 0.0f, 0.0f ) );
        ImGui::Separator();

        DrawToolStrip();
        ImGui::Spacing();
        DrawToolSettings();
        DrawBrushSettings();

        ImGui::PopStyleVar( 2 );
    }

    void LandscapePanel::DrawToolStrip()
    {
        auto&        settings = Core::LandscapeSculptState::Get().Settings;
        // Every button is as wide as the widest name, so "Hydro Erosion" and "Copy/Paste" are not clipped.
        float widest = 0.0f;
        for ( const auto& button : kSculptTools )
            widest = std::max( widest, ImGui::CalcTextSize( Core::LandscapeToolName( button.Tool ) ).x );
        const ImVec2 size( std::max( 72.0f, widest + 2.0f * ImGui::GetStyle().FramePadding.x ), 50.0f );
        const float  right   = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float  spacing = ImGui::GetStyle().ItemSpacing.x;
        for ( size_t i = 0; i < kSculptTools.size(); ++i )
        {
            const auto& button = kSculptTools[i];
            char        label[64];
            std::snprintf( label, sizeof( label ), "%s\n%s##tool%zu", button.Icon,
                           Core::LandscapeToolName( button.Tool ), i );
            if ( AccentButton( label, settings.Tool == button.Tool, size ) )
                settings.Tool = button.Tool;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", Core::LandscapeToolName( button.Tool ) );
            // Wrap like UE's strip: the next button goes on this line only if it fits.
            if ( i + 1 < kSculptTools.size() && ImGui::GetItemRectMax().x + spacing + size.x <= right )
                ImGui::SameLine();
        }
    }

    void LandscapePanel::DrawToolSettings()
    {
        auto& state    = Core::LandscapeSculptState::Get();
        auto& settings = state.Settings;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_COG "  Tool Settings", true,
                                             Core::LandscapeToolName( settings.Tool ) ) )
            return;
        ImGuiUtilities::ResetPropertyRows();
        const auto properties = Core::LandscapeToolProperties();
        for ( size_t i = 0; i < properties.size(); ++i )
        {
            if ( !properties[i].Shown( settings ) )
                continue;
            ImGui::PushID( static_cast<int>( i ) );
            DrawProperty( properties[i], settings );
            ImGui::PopID();
        }

        // The tool's points and buffers (UE draws them in the viewport and in its own detail rows).
        switch ( settings.Tool )
        {
            case Core::LandscapeTool::Ramp:
                PointRow( "Start", state.RampStart, "click the landscape" );
                PointRow( "End", state.RampEnd, "click the landscape" );
                break;
            case Core::LandscapeTool::Mirror:
                PointRow( "Mirror Point", state.MirrorPoint, "landscape centre" );
                break;
            case Core::LandscapeTool::CopyPaste:
                PointRow( "Corner A", state.CopyCornerA, "not set" );
                PointRow( "Corner B", state.CopyCornerB, "not set" );
                ImGuiUtilities::BeginPropertyRow( "Copied" );
                ImGui::Text( "%d x %d samples", state.CopyBuffer.SizeX, state.CopyBuffer.SizeZ );
                ImGuiUtilities::EndPropertyRow();
                break;
            default:
                break;
        }

        // The tool's actions (UE's "Add Ramp" / "Reset", Mirror's Apply, the gizmo's Copy / Paste): the rows
        // of LandscapeToolControls() that post a request, so each button is also a palette command.
        int id = 0;
        for ( const auto& control : Core::LandscapeToolControls() )
        {
            if ( control.Request == Core::LandscapeStrokeRequest::None || !control.Shown( settings ) )
                continue;
            ImGui::PushID( id++ );
            if ( ImGui::Button( control.Label.c_str(), ImVec2( -FLT_MIN, 0.0f ) ) )
                state.Request = control.Request;
            ImGui::PopID();
        }
        // Wrapped: the Ramp hint is wider than the default dock column.
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] );
        ImGui::TextWrapped( "%s", settings.Tool == Core::LandscapeTool::Ramp
                                       ? "LMB sets the start, then the end; Apply builds the ramp"
                                       : "LMB applies the tool, Shift+LMB inverts it" );
        ImGui::PopStyleColor();
    }

    void LandscapePanel::DrawBrushSettings()
    {
        auto& brush = Core::LandscapeSculptState::Get().Settings.Brush;
        if ( !Core::LandscapeToolUsesBrush( Core::LandscapeSculptState::Get().Settings.Tool ) )
            return;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_BRUSH "  Brush Settings" ) )
            return;
        ImGuiUtilities::ResetPropertyRows();

        // UE's brush-set row; the circle is the one set the stroke maths has.
        ImGuiUtilities::BeginPropertyRow( "Brush Type" );
        AccentButton( ICON_MDI_CIRCLE_OUTLINE "  Circle", true, ImVec2( 0.0f, 0.0f ) );
        ImGuiUtilities::EndPropertyRow();

        ImGuiUtilities::BeginPropertyRow( "Brush Falloff Type" );
        int         id      = 0;
        const float right   = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for ( const auto shape : Core::LandscapeFalloffShapes() )
        {
            // Wrap inside the value column: four names do not fit one line of the default dock width.
            const float width = ImGui::CalcTextSize( Core::LandscapeFalloffName( shape ) ).x +
                                2.0f * ImGui::GetStyle().FramePadding.x;
            if ( id > 0 && ImGui::GetItemRectMax().x + spacing + width <= right )
                ImGui::SameLine();
            ImGui::PushID( id++ );
            if ( AccentButton( Core::LandscapeFalloffName( shape ), brush.Shape == shape, ImVec2( 0.0f, 0.0f ) ) )
                brush.Shape = shape;
            ImGui::PopID();
        }
        ImGuiUtilities::EndPropertyRow();

        // BrushRadius / BrushFalloff (LandscapeEditorObject.h:677-690): UI 1..8192 with SliderExponent 3,
        // clamp to 65536; this tool's own floor (kLandscapeMinRadiusCm) is tighter than UE's 1.
        ImGuiUtilities::BeginPropertyRow( "Brush Size" );
        ImGui::SetNextItemWidth( -FLT_MIN );
        if ( ImGui::SliderFloat( "##radius", &brush.RadiusCm, Core::kLandscapeMinRadiusCm, 8192.0f, "%.0f cm",
                                 ImGuiSliderFlags_Logarithmic ) )
            brush.RadiusCm =
                 std::clamp( brush.RadiusCm, Core::kLandscapeMinRadiusCm, Core::kLandscapeMaxRadiusCm );
        ImGuiUtilities::EndPropertyRow();

        ImGuiUtilities::BeginPropertyRow( "Brush Falloff" );
        ImGui::SetNextItemWidth( -FLT_MIN );
        if ( ImGui::SliderFloat( "##falloff", &brush.FalloffFraction, 0.0f, 1.0f, "%.2f" ) )
            brush.FalloffFraction = std::clamp( brush.FalloffFraction, 0.0f, 1.0f );
        ImGuiUtilities::EndPropertyRow();
    }
} // namespace Desert::Editor

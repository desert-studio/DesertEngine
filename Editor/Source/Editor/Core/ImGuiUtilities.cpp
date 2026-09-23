#include "ImGuiUtilities.hpp"

#include <Editor/Core/EditorResources.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>
#include <algorithm>
#include <cstdint>
#include <format>

namespace Desert::Editor::Utils
{
    static int s_UIContextID = 0;

    namespace
    {
        // Vertical extent of the row currently being submitted, so the column rule can be drawn once the
        // columns are open (the background is painted before them).
        float s_RowBandTop    = 0.0f;
        float s_RowBandBottom = 0.0f;

        // Alternating row index, restarted per section so the first row under a header is always the
        // panel colour — as it is in UE.
        int s_PropertyRowIndex = 0;
    } // namespace

    bool ImGuiUtilities::SectionHeader( const char* label, bool defaultOpen, const char* detail )
    {
        // ONE look for every section in the editor. Modelled on UE's Details panel: a flat, full-width
        // grey bar with a disclosure triangle — not a tinted rounded pill. A section header is furniture;
        // when it is coloured and rounded it competes with the actual controls for attention, and when
        // every panel invents its own the whole editor reads as unfinished.
        const ImVec4 bar = ThemeManager::GetSectionHeaderColor();
        ImGui::PushStyleColor( ImGuiCol_Header, bar );
        ImGui::PushStyleColor( ImGuiCol_HeaderHovered,
                               ImVec4( bar.x + 0.05f, bar.y + 0.05f, bar.z + 0.05f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_HeaderActive,
                               ImVec4( bar.x + 0.09f, bar.y + 0.09f, bar.z + 0.09f, 1.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 6.0f, 5.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 0.0f );

        const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth |
                                         ImGuiTreeNodeFlags_AllowItemOverlap |
                                         ( defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0 );

        // NOTE: keep the blank line above — it stops clang-format's consecutive-declaration alignment from
        // padding `open` across the multi-line `flags` init (v18 and v22 disagree on that, tripping the gate).
        const bool open = ImGui::CollapsingHeader( label, flags );

        ImGui::PopStyleVar( 2 );
        ImGui::PopStyleColor( 3 );

        // A section restarts the striping, so the first row under ANY header is the panel colour and never
        // fuses with the bar above it. Doing it here means no call site has to remember.
        s_PropertyRowIndex = 0;

        if ( detail && *detail )
        {
            // Painted into the bar rather than submitted as an item: an item here would sit ON the header
            // and eat the click that opens it.
            const ImVec2 barMin = ImGui::GetItemRectMin();
            const ImVec2 barMax = ImGui::GetItemRectMax();
            const float  textW  = ImGui::CalcTextSize( detail ).x;
            const float  labelW = ImGui::CalcTextSize( label ).x + ImGui::GetFontSize() * 2.0f;
            const float  x      = barMax.x - textW - 8.0f;
            if ( x > barMin.x + labelW )
            {
                const float y = barMin.y + ( barMax.y - barMin.y - ImGui::GetFontSize() ) * 0.5f;
                ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), ImGui::GetColorU32( ImGuiCol_TextDisabled ),
                                                     detail );
            }
        }
        return open;
    }

    void ImGuiUtilities::ResetPropertyRows()
    {
        s_RowBandTop       = 0.0f;
        s_RowBandBottom    = 0.0f;
        s_PropertyRowIndex = 0;
    }

    float ImGuiUtilities::PropertyLabelWidth()
    {
        // The label column follows the panel instead of a fixed width: docked narrow, a fixed column eats
        // the editor and every value box collapses; docked wide, the labels strand far from their values.
        return std::clamp( ImGui::GetWindowWidth() * 0.42f, 110.0f, 230.0f );
    }

    bool ImGuiUtilities::PropertyRowBackground( float height, bool modified )
    {
        const ImGuiStyle& style  = ImGui::GetStyle();
        const ImVec2      rowMin = ImGui::GetCursorScreenPos();
        const float       rowH   = height > 0.0f ? height : ImGui::GetFrameHeight();
        const ImVec2      rowMax( rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + rowH );

        // `AllowWhenBlockedByActiveItem` IS THE WHOLE BUG, AND IT IS A CLICK THAT CANNOT LAND.
        // Without it, IsWindowHovered goes FALSE the instant any widget becomes active — including a
        // button inside this very row. The owner found it from the outside and described the mechanism
        // exactly: "при клике пропадает pin to the top и как будто кликаем в пустоту".
        //
        // The chain: mouse-down on the reset arrow -> that SmallButton is now the active item ->
        // IsWindowHovered false -> rowHovered false -> the pin is not drawn -> `rightEdge` is never
        // stepped left past it -> the reset arrow is laid out one button-width FURTHER RIGHT than it was
        // when it was pressed -> the cursor is no longer over it -> mouse-up lands on nothing and the
        // reset never runs. The button was not broken; it MOVED OUT FROM UNDER THE PRESS.
        //
        // Two rows of buttons whose positions depend on a hover flag that a press destroys is a trap for
        // every future control added here, which is why the flag is fixed rather than the arithmetic.
        const bool hovered = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows |
                                                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) &&
                             ImGui::IsMouseHoveringRect( rowMin, rowMax, /*clip*/ true );

        // Edge to edge, past the window padding: a band that stops short of the border reads as a box
        // instead of a row. The band claims HALF the item spacing on each side, so consecutive rows tile
        // with no gap between them — that continuous surface is what makes UE's grid read as a grid.
        const float  half = style.ItemSpacing.y * 0.5f;
        const ImVec2 bandMin( rowMin.x - style.WindowPadding.x, rowMin.y - half );
        const ImVec2 bandMax( rowMax.x + style.WindowPadding.x, rowMax.y + half );
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        // Zebra AND a rule, both measured off the UE reference: odd rows are painted in the section-bar
        // grey (#2F2F2F) over the panel's #242424, and every row still gets its 1px #1A1A1A rule. The
        // Static Mesh Editor's own Details happens to show uniform rows, but the Blueprint one alternates,
        // and the alternation is what makes a long list of slots readable — a value belongs to the band it
        // sits in, not to the label that happens to be closest.
        //
        // Index 0 is deliberately UNPAINTED: a section header is that same grey, so a painted first row
        // would fuse with the bar above it.
        if ( hovered )
        {
            dl->AddRectFilled( bandMin, bandMax, IM_COL32( 255, 255, 255, 12 ) );
        }
        else if ( ( s_PropertyRowIndex & 1 ) != 0 )
        {
            dl->AddRectFilled( bandMin, bandMax, ImGui::GetColorU32( ThemeManager::GetSectionHeaderColor() ) );
        }
        dl->AddLine( ImVec2( bandMin.x, bandMin.y ), ImVec2( bandMax.x, bandMin.y ),
                     ImGui::GetColorU32( ImGuiCol_Border ) );

        // "Not the default" as a 2px edge on the band, drawn AFTER the fill so the stripe cannot hide it.
        // The accent, not the selection blue: this reports a VALUE, which is the line the theme draws
        // between its two blues (see ThemeManager::SetDarkTheme).
        if ( modified )
        {
            dl->AddRectFilled( bandMin, ImVec2( bandMin.x + 2.0f, bandMax.y ),
                               ImGui::GetColorU32( ThemeManager::GetSelectedColor() ) );
        }

        ++s_PropertyRowIndex;

        s_RowBandTop    = bandMin.y;
        s_RowBandBottom = bandMax.y;
        return hovered;
    }

    void ImGuiUtilities::PropertyColumnRule()
    {
        if ( s_RowBandBottom <= s_RowBandTop )
            return;

        // GetColumnOffset is measured from the window's content edge, which is exactly where the label
        // column starts — so the rule lands on the real split whatever the indent.
        const float x = ImGui::GetWindowPos().x - ImGui::GetScrollX() + ImGui::GetColumnOffset( 1 ) -
                        ImGui::GetStyle().ItemSpacing.x * 0.5f;
        ImGui::GetWindowDrawList()->AddLine( ImVec2( x, s_RowBandTop ), ImVec2( x, s_RowBandBottom ),
                                             ImGui::GetColorU32( ImGuiCol_Border ) );
    }

    void ImGuiUtilities::BeginPropertyRow( const char* label, const char* tooltip, float height )
    {
        PropertyRowBackground( height );

        ImGui::Columns( 2 );
        ImGui::SetColumnWidth( 0, PropertyLabelWidth() );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        if ( tooltip )
            Tooltip( tooltip );

        ImGui::NextColumn();
        ImGui::PushItemWidth( -1.0f );
    }

    void ImGuiUtilities::EndPropertyRow()
    {
        ImGui::PopItemWidth();
        ImGui::NextColumn();
        PropertyColumnRule();
        ImGui::Columns( 1 );
    }

    bool ImGuiUtilities::AccentButton( const char* label, float height )
    {
        // The theme's accent, not a second blue of its own — the editor gets ONE primary colour.
        const ImVec4 accent = ThemeManager::GetSelectedColor();
        ImGui::PushStyleColor( ImGuiCol_Button, accent );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered,
                               ImVec4( accent.x + 0.10f, accent.y + 0.08f, accent.z + 0.06f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive,
                               ImVec4( accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.0f );
        const bool clicked = ImGui::Button( label, ImVec2( ImGui::GetContentRegionAvail().x, height ) );
        ImGui::PopStyleVar();
        ImGui::PopStyleColor( 3 );
        return clicked;
    }

    bool ImGuiUtilities::EmptyState( const char* icon, const char* title, const char* body, const char* action )
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if ( avail.x <= 0.0f || avail.y <= 0.0f )
            return false;

        // Slightly above centre, the way an empty state sits in UE and in every file dialog: dead centre
        // reads as "loading", a little high reads as "this is the content".
        ImGui::Dummy( ImVec2( 0.0f, avail.y * 0.22f ) );

        const auto centred = [avail]( float width )
        { ImGui::SetCursorPosX( ImGui::GetCursorPosX() + std::max( 0.0f, ( avail.x - width ) * 0.5f ) ); };

        // The glyph is the thing that reads at a glance, so it gets the big icon font and a colour well
        // below the body text: it is a mood, not information.
        if ( ImFont* big = EditorResources::GetBigIconFont() )
            ImGui::PushFont( big );
        centred( ImGui::CalcTextSize( icon ).x );
        ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.42f, 0.42f, 0.42f, 1.0f ) );
        ImGui::TextUnformatted( icon );
        ImGui::PopStyleColor();
        if ( EditorResources::GetBigIconFont() )
            ImGui::PopFont();

        ImGui::Dummy( ImVec2( 0.0f, 8.0f ) );
        if ( ImFont* bold = EditorResources::GetBoldFont() )
            ImGui::PushFont( bold );
        centred( ImGui::CalcTextSize( title ).x );
        ImGui::TextUnformatted( title );
        if ( EditorResources::GetBoldFont() )
            ImGui::PopFont();

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        // Wrapped to a column narrower than the dock: a sentence running the full width of a wide panel
        // is read as a paragraph and skipped.
        const float wrap = std::min( avail.x - 32.0f, 300.0f );
        if ( wrap > 40.0f )
        {
            centred( wrap );
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + wrap );
            ImGui::TextWrapped( "%s", body );
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        bool clicked = false;
        if ( action )
        {
            ImGui::Dummy( ImVec2( 0.0f, 10.0f ) );
            const float buttonW = ImGui::CalcTextSize( action ).x + ImGui::GetFontSize() * 2.0f;
            centred( buttonW );
            // AccentButton takes the full remaining width, so it is fenced into a child of exactly the
            // width we want rather than reimplemented with a second set of accent colours.
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
            ImGui::BeginChild( "##EmptyStateAction", ImVec2( buttonW, ImGui::GetFrameHeight() ), false,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
            clicked = AccentButton( action );
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        return clicked;
    }

    bool ImGuiUtilities::VectorField( const char* id, float* values, int components, float speed,
                                      const char* format )
    {
        // The theme's axis colours — the same three the viewport gizmo and the CubeGrid handles use. A
        // fourth component (W) is deliberately neutral: it is not an axis in space.

        if ( components <= 0 )
            return false;

        constexpr float kSpacing = 4.0f;
        constexpr float kEdge    = 3.0f;

        const float total = ImGui::CalcItemWidth();
        const float each =
             ( total - kSpacing * static_cast<float>( components - 1 ) ) / static_cast<float>( components );

        bool changed = false;
        ImGui::PushID( id );
        for ( int i = 0; i < components; ++i )
        {
            ImGui::PushID( i );
            ImGui::SetNextItemWidth( each );
            if ( ImGui::DragFloat( "##v", &values[i], speed, 0.0f, 0.0f, format ) )
                changed = true;

            // Painted after the field, so it sits ON the frame instead of being covered by it.
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                 mn, ImVec2( mn.x + kEdge, mx.y ), ImGui::GetColorU32( ThemeManager::GetAxisColor( i ) ),
                 ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft );
            ImGui::PopID();

            if ( i + 1 < components )
                ImGui::SameLine( 0.0f, kSpacing );
        }
        ImGui::PopID();
        return changed;
    }

    bool ImGuiUtilities::AssetSlot( const char* id, const char* text, bool empty )
    {
        ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_FrameBg ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4( ImGuiCol_FrameBgHovered ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4( ImGuiCol_FrameBgActive ) );
        ImGui::PushStyleColor( ImGuiCol_Text,
                               ImGui::GetStyleColorVec4( empty ? ImGuiCol_TextDisabled : ImGuiCol_Text ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.0f, 0.5f ) );

        // Room for the chevron, so a long asset name is clipped by the button instead of running under it.
        const float chevron = ImGui::GetFontSize() + 6.0f;
        ImGui::PushID( id );
        const bool clicked = ImGui::Button( text, ImVec2( ImGui::GetContentRegionAvail().x, 0.0f ) );
        ImGui::PopID();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor( 4 );

        const ImVec2 slotMin = ImGui::GetItemRectMin();
        const ImVec2 slotMax = ImGui::GetItemRectMax();
        const float  y       = slotMin.y + ( slotMax.y - slotMin.y - ImGui::GetFontSize() ) * 0.5f;
        ImGui::GetWindowDrawList()->AddText( ImVec2( slotMax.x - chevron, y ),
                                             ImGui::GetColorU32( ImGuiCol_TextDisabled ), ICON_MDI_CHEVRON_DOWN );
        return clicked;
    }

    void ImGuiUtilities::PushID()
    {
        ImGui::PushID( s_UIContextID++ );
    }

    void ImGuiUtilities::PopID()
    {
        ImGui::PopID();
        s_UIContextID--;
    }

    bool ImGuiUtilities::InputText( std::string& currentText, const char* ID )
    {
        ImGui::PushStyleColor( ImGuiCol_FrameBg, IM_COL32( 0, 0, 0, 0 ) );
        ImGuiUtilities::DrawItemActivityOutline( 2.0f, false, ImColor( 80, 80, 80 ) );

        ImGui::PushID( ID );

        bool edited = ImGui::InputText(
             ID, currentText.data(), currentText.size() + 1, ImGuiInputTextFlags_CallbackResize,
             []( ImGuiInputTextCallbackData* data ) -> int
             {
                 if ( data->EventFlag == ImGuiInputTextFlags_CallbackResize )
                 {
                     std::string* str = static_cast<std::string*>( data->UserData );
                     str->resize( data->BufTextLen );
                     data->Buf = str->data();
                 }
                 return 0;
             },
             &currentText );

        ImGui::PopID();
        ImGui::PopStyleColor();

        return edited;
    }

    void ImGuiUtilities::DrawItemActivityOutline( float rounding, bool drawWhenInactive, ImColor colourWhenActive )
    {
        auto* drawList = ImGui::GetWindowDrawList();

        ImRect expandedRect = ImRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );
        expandedRect.Min.x -= 1.0f;
        expandedRect.Min.y -= 1.0f;
        expandedRect.Max.x += 1.0f;
        expandedRect.Max.y += 1.0f;

        const ImRect rect = expandedRect;
        if ( ImGui::IsItemHovered() && !ImGui::IsItemActive() )
        {
            drawList->AddRect( rect.Min, rect.Max, ImColor( 60, 60, 60 ), rounding, 0, 1.5f );
        }
        if ( ImGui::IsItemActive() )
        {
            drawList->AddRect( rect.Min, rect.Max, colourWhenActive, rounding, 0, 1.0f );
        }
        else if ( !ImGui::IsItemHovered() && drawWhenInactive )
        {
            drawList->AddRect( rect.Min, rect.Max, ImColor( 50, 50, 50 ), rounding, 0, 1.0f );
        }
    };

    void ImGuiUtilities::Tooltip( const char* text )
    {
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 5, 5 ) );

        if ( ImGui::IsItemHovered() )
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted( text );
            ImGui::EndTooltip();
        }

        ImGui::PopStyleVar();
    }

    bool ImGuiUtilities::Property( const char* name, uint32_t& value, ImGuiUtilities::PropertyFlag flags )
    {
        bool updated = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( name );
        ImGui::NextColumn();
        ImGui::PushItemWidth( -1 );

        if ( (int)flags & (int)PropertyFlag::ReadOnly )
        {
            ImGui::Text( "%d", value );
        }
        else
        {
            std::string id = "##" + std::string( name );
            updated        = ImGui::DragScalar( id.c_str(), ImGuiDataType_U32, &value );
        }
        ImGui::PopItemWidth();
        ImGui::NextColumn();

        return updated;
    }

    bool ImGuiUtilities::Property( const char* name, bool& value, PropertyFlag flags )
    {
        bool updated = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( name );
        ImGui::NextColumn();
        ImGui::PushItemWidth( -1 );

        if ( (int)flags & (int)PropertyFlag::ReadOnly )
        {
            ImGui::TextUnformatted( value ? "True" : "False" );
        }
        else
        {
            std::string id = "##" + std::string( name );
            if ( ImGui::Checkbox( id.c_str(), &value ) )
                updated = true;
        }

        ImGui::PopItemWidth();
        ImGui::NextColumn();

        return updated;
    }

    bool ImGuiUtilities::Property( const char* name, std::string& value, PropertyFlag flags )
    {
        bool updated = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( name );
        ImGui::NextColumn();
        ImGui::PushItemWidth( -1 );
        ImGui::AlignTextToFramePadding();

        if ( (int)flags & (int)PropertyFlag::ReadOnly )
        {
            ImGui::TextUnformatted( value.c_str() );
        }
        else
        {
            if ( ImGuiUtilities::InputText( value, name ) )
            {
                updated = true;
            }
        }
        ImGui::PopItemWidth();
        ImGui::NextColumn();

        return updated;
    }

    bool ImGuiUtilities::Property( const char* name, float& value, float min /*= -1.0f*/, float max /*= 1.0f*/,
                                   float delta /*= 1.0f*/, PropertyFlag flags /*= PropertyFlag::None */ )
    {
        bool updated = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( name );
        ImGui::NextColumn();
        ImGui::PushItemWidth( -1 );

        if ( (int)flags & (int)PropertyFlag::ReadOnly )
        {
            ImGui::Text( "%.2f", value );
        }
        else if ( (int)flags & (int)PropertyFlag::DragValue )
        {
            if ( ImGui::DragFloat( std::format( "##{}", name ).c_str(), &value, delta, min, max ) )
                updated = true;
        }
        else if ( (int)flags & (int)PropertyFlag::SliderValue )
        {
            if ( ImGui::SliderFloat( std::format( "##{}", name ).c_str(), &value, min, max ) )
                updated = true;
        }
        else
        {
            if ( ImGui::InputFloat( std::format( "##{}", name ).c_str(), &value, delta ) )
                updated = true;
        }
        ImGui::PopItemWidth();
        ImGui::NextColumn();

        return updated;
    }

    // NO `min`/`max`, AND NO `exposeW`. Both were parameters of this function and neither did anything:
    // `min` and `max` were never passed to `DragFloat3` (the float overload above DOES pass them to
    // `DragFloat`/`SliderFloat`, which is what makes the omission here a defect rather than a style), and
    // the `exposeW` ternary below had IDENTICAL branches — a leftover of a vec4 widget, on a type with no
    // w. `-Wunused-parameter` named `min` and `max`; `exposeW` escaped only because a ternary condition
    // counts as a use.
    //
    // Removed rather than wired up: the whole overload set has exactly two call sites in the editor and
    // both take the `std::string` one, so there is no caller whose range this would start honouring and
    // no evidence of what range anyone wanted. A clamp added on a guess is a slider that moves the wrong
    // amount, which is worse than one that does not clamp.
    bool ImGuiUtilities::Property( const char* name, glm::vec3& value, PropertyFlag flags )
    {
        bool updated = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( name );
        ImGui::NextColumn();
        ImGui::PushItemWidth( -1 );
        if ( (int)flags & (int)PropertyFlag::ReadOnly )
        {
            ImGui::Text( "%.2f , %.2f, %.2f", value.x, value.y, value.z );
        }
        else
        {

            // std::string id = "##" + name;
            if ( (int)flags & (int)PropertyFlag::ColorProperty )
            {
                if ( ImGui::ColorEdit3( std::format( "##{}", name ).c_str(), &value.x ) )
                    updated = true;
            }
            else if ( ImGui::DragFloat3( std::format( "##{}", name ).c_str(), &value.x ) )
                updated = true;
        }
        ImGui::PopItemWidth();
        ImGui::NextColumn();

        return updated;
    }

    ImRect ImGuiUtilities::RectExpanded( const ImRect& rect, float x, float y )
    {
        ImRect result = rect;
        result.Min.x -= x;
        result.Min.y -= y;
        result.Max.x += x;
        result.Max.y += y;
        return result;
    }

    void ImGuiUtilities::DrawBorder( ImVec2 rectMin, ImVec2 rectMax, const ImVec4& borderColour, float thickness,
                                     float offsetX, float offsetY )
    {
        auto min = rectMin;
        min.x -= thickness;
        min.y -= thickness;
        min.x += offsetX;
        min.y += offsetY;
        auto max = rectMax;
        max.x += thickness;
        max.y += thickness;
        max.x += offsetX;
        max.y += offsetY;

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect( min, max, ImGui::ColorConvertFloat4ToU32( borderColour ), 0.0f, 0, thickness );
    }

    void ImGuiUtilities::DrawBorder( ImRect rect, float thickness, float rounding, float offsetX, float offsetY )
    {
        auto min = rect.Min;
        min.x -= thickness;
        min.y -= thickness;
        min.x += offsetX;
        min.y += offsetY;
        auto max = rect.Max;
        max.x += thickness;
        max.y += thickness;
        max.x += offsetX;
        max.y += offsetY;

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect( min, max, ImGui::ColorConvertFloat4ToU32( ImGui::GetStyleColorVec4( ImGuiCol_Border ) ),
                           rounding, 0, thickness );
    }

    ImRect ImGuiUtilities::GetItemRect()
    {
        return ImRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );
    }
} // namespace Desert::Editor::Utils
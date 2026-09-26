#include "WindowChrome.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Widgets/WindowButtonStyle.hpp>
#include <Editor/Widgets/WindowResizeMath.hpp>

#include <Engine/Core/Window.hpp>

#include <ImGui/imgui_internal.h>

#include <algorithm>
#include <string>

namespace Desert::Editor::UI
{
    namespace
    {
        // A grip is 6 logical pixels. Wider reads as a dead border around the editor; narrower is the
        // classic "I cannot grab the edge" complaint. Windows' own frame grip is 4 device pixels plus a
        // 4px invisible margin, which is where the number comes from.
        constexpr float kGripThickness = 6.0f;

        ImVec4 ToImGui( const ButtonColour& colour )
        {
            return ImVec4( colour.R, colour.G, colour.B, colour.A );
        }

        /// One window button. Its own colours rather than the shared ToolbarButton, because close must go
        /// red on hover and nothing else in the editor does that. The colours, width and glyphs are the
        /// ones the start-up splash draws with too (WindowButtonStyle.hpp).
        bool WindowButton( const char* id, const char* icon, const char* tooltip, bool danger )
        {
            const ImVec4 hovered = ToImGui( danger ? kCloseButtonHovered : kWindowButtonHovered );
            const ImVec4 active  = ToImGui( danger ? kCloseButtonPressed : kWindowButtonPressed );

            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, hovered );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, active );
            ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 0.0f );

            const float height  = ImGui::GetFrameHeight();
            const bool  clicked = ImGui::Button( ( std::string( icon ) + "##" + id ).c_str(),
                                                 ImVec2( kWindowButtonWidth, height ) );

            ImGui::PopStyleVar();
            ImGui::PopStyleColor( 3 );

            if ( tooltip && ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", tooltip );

            return clicked;
        }
    } // namespace

    float WindowChrome::WindowButtonsWidth()
    {
        return kWindowButtonWidth * 3.0f;
    }

    float WindowChrome::DrawWindowButtons()
    {
        const float width = WindowButtonsWidth();

        ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - width );

        // ItemSpacing to zero so the three squares touch, as every platform's window buttons do; a gap
        // between them would read as three unrelated controls.
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0.0f, 0.0f ) );

        if ( WindowButton( "min", kMinimizeGlyph, "Minimize", false ) )
            m_Window.Minimize();
        ImGui::SameLine();

        // THE ICON IS CHOSEN FROM THE OS'S ANSWER, not from a flag of ours: ask every frame, so a window
        // maximized by anything else (a keyboard shortcut, a window manager, the OS restoring a session)
        // is drawn as maximized here too.
        const bool maximized = m_Window.IsWindowMaximized();
        if ( WindowButton( "max", maximized ? ICON_MDI_WINDOW_RESTORE : ICON_MDI_WINDOW_MAXIMIZE,
                           maximized ? "Restore" : "Maximize", false ) )
        {
            if ( maximized )
                m_Window.Restore();
            else
                m_Window.Maximize();
        }
        ImGui::SameLine();

        if ( WindowButton( "close", kCloseGlyph, "Close the editor", true ) && m_OnCloseRequested )
            m_OnCloseRequested();

        ImGui::PopStyleVar();
        return width;
    }

    void WindowChrome::HandleTitleBarGestures()
    {
        ImGuiIO& io = ImGui::GetIO();

        // "Over the bar and over nothing on it". IsAnyItemHovered covers the menus and the window buttons;
        // IsWindowHovered( ChildWindows ) is what makes the bar's own background the subject.
        const bool overEmptyBar =
             ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) && !ImGui::IsAnyItemHovered();

        // Double-click first: a double click also reports a single click, and starting a drag on the way to
        // toggling would move the window by a few pixels every time somebody maximized it.
        if ( overEmptyBar && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
        {
            m_Dragging = false;
            if ( m_Window.IsWindowMaximized() )
                m_Window.Restore();
            else
                m_Window.Maximize();
            return;
        }

        if ( overEmptyBar && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            // Dragging a MAXIMIZED window restores it and carries on, which is what both platforms' own
            // title bars do. The window is then placed so the cursor stays at the same fraction across the
            // bar — dropping the restored window at its old position would tear it out from under the
            // pointer, and the user would be dragging thin air.
            if ( m_Window.IsWindowMaximized() )
            {
                const float fraction =
                     ImGui::GetMainViewport()->Size.x > 0.0f
                          ? ( io.MousePos.x - ImGui::GetMainViewport()->Pos.x ) / ImGui::GetMainViewport()->Size.x
                          : 0.5f;
                m_Window.Restore();

                const int restoredW = (int)m_Window.GetWidth();
                m_Window.SetWindowPos( (int)( io.MousePos.x - fraction * (float)restoredW ),
                                       (int)( io.MousePos.y - ImGui::GetFrameHeight() * 0.5f ) );
            }

            m_Dragging       = true;
            m_DragMouseStart = io.MousePos;
            m_Window.GetWindowPos( m_DragWindowX, m_DragWindowY );
        }

        if ( m_Dragging && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            m_Dragging = false;

        if ( m_Dragging )
        {
            m_Window.SetWindowPos( m_DragWindowX + (int)( io.MousePos.x - m_DragMouseStart.x ),
                                   m_DragWindowY + (int)( io.MousePos.y - m_DragMouseStart.y ) );
        }
    }

    void WindowChrome::DrawResizeBorders()
    {
        // A maximized window has no edges to drag: a grip there would silently un-maximize it, which is
        // the "two owners of one fact" defect wearing a mouse cursor. Both platforms' own frames behave
        // the same way.
        if ( m_Window.IsWindowMaximized() )
        {
            m_ResizeX = m_ResizeY = 0;
            return;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2         pos      = viewport->Pos;
        const ImVec2         size     = viewport->Size;
        const float          g        = kGripThickness;

        // The eight grips, as (dx, dy) sign pairs. dx/dy of -1 means this grip moves the left/top edge (so
        // the window's position moves with it), +1 the right/bottom edge, 0 leaves that axis alone.
        struct Grip
        {
            int              dx;
            int              dy;
            ImVec2           pos;
            ImVec2           size;
            ImGuiMouseCursor cursor;
        };
        const Grip grips[] = {
             { -1, 0, ImVec2( pos.x, pos.y + g ), ImVec2( g, size.y - 2 * g ), ImGuiMouseCursor_ResizeEW },
             { +1, 0, ImVec2( pos.x + size.x - g, pos.y + g ), ImVec2( g, size.y - 2 * g ),
               ImGuiMouseCursor_ResizeEW },
             { 0, -1, ImVec2( pos.x + g, pos.y ), ImVec2( size.x - 2 * g, g ), ImGuiMouseCursor_ResizeNS },
             { 0, +1, ImVec2( pos.x + g, pos.y + size.y - g ), ImVec2( size.x - 2 * g, g ),
               ImGuiMouseCursor_ResizeNS },
             { -1, -1, ImVec2( pos.x, pos.y ), ImVec2( g, g ), ImGuiMouseCursor_ResizeNWSE },
             { +1, +1, ImVec2( pos.x + size.x - g, pos.y + size.y - g ), ImVec2( g, g ),
               ImGuiMouseCursor_ResizeNWSE },
             { +1, -1, ImVec2( pos.x + size.x - g, pos.y ), ImVec2( g, g ), ImGuiMouseCursor_ResizeNESW },
             { -1, +1, ImVec2( pos.x, pos.y + size.y - g ), ImVec2( g, g ), ImGuiMouseCursor_ResizeNESW },
        };

        constexpr ImGuiWindowFlags kGripFlags =
             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        // ImGui refuses to make a window smaller than style.WindowMinSize, and a 6px strip is smaller than
        // any sane default. Without this the grips would be 32px slabs over the panels behind them.
        ImGui::PushStyleVar( ImGuiStyleVar_WindowMinSize, ImVec2( 1.0f, 1.0f ) );

        for ( int i = 0; i < (int)( sizeof( grips ) / sizeof( grips[0] ) ); ++i )
        {
            const Grip& grip = grips[i];
            if ( grip.size.x <= 0.0f || grip.size.y <= 0.0f )
                continue;

            char name[32];
            ImFormatString( name, sizeof( name ), "##WindowGrip%d", i );

            ImGui::SetNextWindowPos( grip.pos );
            ImGui::SetNextWindowSize( grip.size );
            ImGui::SetNextWindowViewport( viewport->ID );
            if ( ImGui::Begin( name, nullptr, kGripFlags ) )
            {
                ImGui::InvisibleButton( "grip", grip.size );
                if ( ImGui::IsItemHovered() || ImGui::IsItemActive() )
                    ImGui::SetMouseCursor( grip.cursor );

                if ( ImGui::IsItemActivated() )
                {
                    m_ResizeX          = grip.dx;
                    m_ResizeY          = grip.dy;
                    m_ResizeMouseStart = ImGui::GetIO().MousePos;
                    m_Window.GetWindowPos( m_ResizeStartPosX, m_ResizeStartPosY );
                    m_ResizeStartW = (int)m_Window.GetWidth();
                    m_ResizeStartH = (int)m_Window.GetHeight();
                }
            }
            ImGui::End();
        }

        ImGui::PopStyleVar( 3 );

        if ( ( m_ResizeX != 0 || m_ResizeY != 0 ) && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
        {
            m_ResizeX = m_ResizeY = 0;
            return;
        }
        if ( m_ResizeX == 0 && m_ResizeY == 0 )
            return;

        // A floor, because a window of zero width is a swapchain of zero width: the Vulkan surface refuses
        // an extent with a zero in it, and the frame after that is a stream of refusals rather than a small
        // window. 320x240 is arbitrary but it is a size the editor's own bar still fits in.
        constexpr int kMinW = 320;
        constexpr int kMinH = 240;

        const WindowRect start = { m_ResizeStartPosX, m_ResizeStartPosY, m_ResizeStartW, m_ResizeStartH };
        const WindowRect moved = ResizeFromGrip(
             start, m_ResizeX, m_ResizeY, (int)( ImGui::GetIO().MousePos.x - m_ResizeMouseStart.x ),
             (int)( ImGui::GetIO().MousePos.y - m_ResizeMouseStart.y ), kMinW, kMinH );

        if ( moved.X != start.X || moved.Y != start.Y )
            m_Window.SetWindowPos( moved.X, moved.Y );
        if ( moved.W != (int)m_Window.GetWidth() || moved.H != (int)m_Window.GetHeight() )
            m_Window.SetWindowSize( (uint32_t)moved.W, (uint32_t)moved.H );
    }
} // namespace Desert::Editor::UI

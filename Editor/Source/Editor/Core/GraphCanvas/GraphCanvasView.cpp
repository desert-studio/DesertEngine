#include "GraphCanvasView.hpp"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <vector>

namespace ed = ax::NodeEditor;

namespace Desert::Editor::Graph
{
    namespace
    {
        // The node editor's API is a global-current-context one, so every entry point here brackets its
        // own use of it. Leaving a context current is how one canvas ends up answering another one's
        // question about what is selected.
        class ScopedEditor
        {
        public:
            explicit ScopedEditor( ed::EditorContext* context )
            {
                ed::SetCurrentEditor( context );
            }

            ~ScopedEditor()
            {
                ed::SetCurrentEditor( nullptr );
            }

            ScopedEditor( const ScopedEditor& )            = delete;
            ScopedEditor& operator=( const ScopedEditor& ) = delete;
        };

        constexpr float kFrameAllDuration = 0.4f; // seconds of animation; 0 would teleport the view

        constexpr float kCommentPadding = 24.0f;  // room between a framed node and the box's edge
        constexpr float kCommentWidth   = 320.0f; // a box made with nothing selected
        constexpr float kCommentHeight  = 180.0f;

        /// The canvas distance from a comment node's top-left to its framed area: the node padding and
        /// one title line. `NewCommentRect` places the node so the AREA, not the title, wraps the nodes.
        ImVec2 CommentTitleOffset()
        {
            const ImVec4 padding = ed::GetStyle().NodePadding; // x = left, y = top
            return ImVec2( padding.x, padding.y + ImGui::GetTextLineHeightWithSpacing() );
        }

        /// The keyboard belongs to this canvas: its window (or a child) is focused and nothing is typed into.
        bool CanvasHasKeyboard()
        {
            return ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && !ImGui::IsAnyItemActive() &&
                   !ImGui::GetIO().WantTextInput;
        }
    } // namespace

    void FrameAll( ed::EditorContext* context )
    {
        if ( context == nullptr )
            return;
        const ScopedEditor scope( context );
        ed::NavigateToContent( kFrameAllDuration );
    }

    void FrameSelection( ed::EditorContext* context )
    {
        if ( context == nullptr )
            return;
        const ScopedEditor scope( context );

        // NOTHING SELECTED FRAMES EVERYTHING. `NavigateToSelection` on an empty selection navigates to an
        // empty rectangle, which is a view from which the graph is not visible and from which the user
        // would need the very key they just pressed.
        if ( ed::GetSelectedObjectCount() > 0 )
            ed::NavigateToSelection( false, kFrameAllDuration );
        else
            ed::NavigateToContent( kFrameAllDuration );
    }

    void PushNodePosition( const PlannedNode& node )
    {
        if ( !node.PushPosition )
            return;
        ed::SetNodePosition( ed::NodeId( Raw( node.Id ) ), ImVec2( node.X, node.Y ) );
    }

    bool PullNodePosition( const PlannedNode& node, float& x, float& y )
    {
        const ImVec2 position = ed::GetNodePosition( ed::NodeId( Raw( node.Id ) ) );
        if ( position.x == x && position.y == y )
            return false;
        x = position.x;
        y = position.y;
        return true;
    }

    void DrawViewButtons( ed::EditorContext* context )
    {
        if ( ImGui::Button( "Frame All" ) )
            FrameAll( context );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Move the view so the whole graph fits (F)" );

        ImGui::SameLine();
        if ( ImGui::Button( "Frame Sel" ) )
            FrameSelection( context );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Move the view to what is selected, or to everything when nothing is (Shift+F)" );

        // THE KEYS, AND ONLY WHILE THIS DOCUMENT HAS THE KEYBOARD. `ImGui::IsWindowFocused` with the
        // child-windows flag is what makes "F" mean this canvas rather than every open canvas at once.
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && !ImGui::IsAnyItemActive() )
        {
            // Ctrl+F is Find…, not Frame All.
            if ( ImGui::IsKeyPressed( ImGuiKey_F, false ) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper )
            {
                if ( ImGui::GetIO().KeyShift )
                    FrameSelection( context );
                else
                    FrameAll( context );
            }
        }
    }

    void DrawStatusLine( const std::string& status, bool isError )
    {
        if ( status.empty() )
            return;
        ImGui::SameLine();
        ImGui::TextColored( isError ? ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ) : ImVec4( 0.5f, 0.9f, 0.5f, 1.0f ), "%s",
                            status.c_str() );
    }

    bool DrawCommentBox( ElementId id, bool fresh, const std::string& text, float& x, float& y, float& width,
                         float& height )
    {
        const ed::NodeId node( Raw( id ) );
        if ( fresh )
        {
            ed::SetNodePosition( node, ImVec2( x, y ) );
            ed::SetGroupSize( node, ImVec2( width, height ) );
        }

        ed::PushStyleColor( ed::StyleColor_NodeBg, ImColor( 255, 255, 255, 40 ) );
        ed::PushStyleColor( ed::StyleColor_NodeBorder, ImColor( 255, 255, 255, 64 ) );
        ed::BeginNode( node );
        ImGui::TextUnformatted( text.empty() ? "Comment" : text.c_str() );
        // A group already known to the canvas keeps the size the user dragged it to; the argument is read
        // only the frame the group is made, which `SetGroupSize` above covers for a fresh box.
        ed::Group( ImVec2( width, height ) );
        const ImVec2 area = ImGui::GetItemRectSize(); // the framed area as the canvas holds it now
        ed::EndNode();
        ed::PopStyleColor( 2 );

        if ( fresh )
            return false;
        const ImVec2 at      = ed::GetNodePosition( node );
        const bool   changed = at.x != x || at.y != y || area.x != width || area.y != height;
        x                    = at.x;
        y                    = at.y;
        width                = area.x;
        height               = area.y;
        return changed;
    }

    CanvasRect NewCommentRect()
    {
        std::vector<CanvasRect> selected;
        const int               count = ed::GetSelectedObjectCount();
        if ( count > 0 )
        {
            std::vector<ed::NodeId> nodes( static_cast<size_t>( count ) );
            nodes.resize( static_cast<size_t>( ed::GetSelectedNodes( nodes.data(), count ) ) );
            for ( const ed::NodeId node : nodes )
            {
                const ImVec2 at   = ed::GetNodePosition( node );
                const ImVec2 size = ed::GetNodeSize( node );
                selected.push_back( CanvasRect{ at.x, at.y, size.x, size.y } );
            }
        }

        const ImVec2 offset = CommentTitleOffset();
        if ( const std::optional<CanvasRect> area = EncloseRects( selected, kCommentPadding ) )
            return CanvasRect{ area->X - offset.x, area->Y - offset.y, area->Width, area->Height };

        const ImVec2 mouse = ed::ScreenToCanvas( ImGui::GetMousePos() );
        return CanvasRect{ mouse.x, mouse.y, kCommentWidth, kCommentHeight };
    }

    bool CommentKeyPressed()
    {
        const ImGuiIO& io = ImGui::GetIO();
        return CanvasHasKeyboard() && !io.KeyCtrl && !io.KeySuper && ImGui::IsKeyPressed( ImGuiKey_C, false );
    }

    bool FindKeyPressed()
    {
        const ImGuiIO& io = ImGui::GetIO();
        return CanvasHasKeyboard() && ( io.KeyCtrl || io.KeySuper ) && ImGui::IsKeyPressed( ImGuiKey_F, false );
    }

    void FocusNode( ElementId id )
    {
        if ( id == ElementId::Invalid )
            return;
        ed::SelectNode( ed::NodeId( Raw( id ) ), false );
        ed::NavigateToSelection( false, kFrameAllDuration );
    }

    void FindPopup::Open()
    {
        m_OpenRequested = true;
        m_FocusQuery    = true;
    }

    std::optional<size_t> FindPopup::Draw( std::span<const std::string> names )
    {
        if ( m_OpenRequested )
        {
            ImGui::OpenPopup( m_PopupId.c_str() );
            m_OpenRequested = false;
        }
        if ( !ImGui::BeginPopup( m_PopupId.c_str() ) )
            return std::nullopt;

        std::optional<size_t> picked;
        if ( m_FocusQuery )
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusQuery = false;
        }
        char buffer[256] = {};
        m_Query.copy( buffer, sizeof( buffer ) - 1 );
        ImGui::SetNextItemWidth( 240.0f );
        const bool enter = ImGui::InputTextWithHint( "##find", "Find node…", buffer, sizeof( buffer ),
                                                     ImGuiInputTextFlags_EnterReturnsTrue );
        m_Query          = buffer;

        const std::vector<size_t> matches = FindMatches( names, m_Query );
        if ( matches.empty() )
            ImGui::TextDisabled( "No node is named like '%s'", m_Query.c_str() );
        for ( const size_t index : matches )
            if ( ImGui::Selectable( names[index].c_str() ) )
                picked = index;
        // ENTER TAKES THE FIRST MATCH, so a typed name is one keystroke from its node.
        if ( enter && !matches.empty() )
            picked = matches.front();
        if ( picked )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return picked;
    }

    bool CommentTextPopup( const char* popupId, std::string& text )
    {
        if ( !ImGui::BeginPopup( popupId ) )
            return false;
        if ( ImGui::IsWindowAppearing() )
            ImGui::SetKeyboardFocusHere();
        char buffer[512] = {};
        text.copy( buffer, sizeof( buffer ) - 1 );
        ImGui::SetNextItemWidth( 280.0f );
        const bool done =
             ImGui::InputText( "##commentText", buffer, sizeof( buffer ), ImGuiInputTextFlags_EnterReturnsTrue );
        const bool changed = text != buffer;
        text               = buffer;
        if ( done )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return changed;
    }
} // namespace Desert::Editor::Graph

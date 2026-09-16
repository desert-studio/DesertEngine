#include "GraphCanvasView.hpp"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

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
            if ( ImGui::IsKeyPressed( ImGuiKey_F, false ) )
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
        ImGui::TextColored( isError ? ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ) : ImVec4( 0.5f, 0.9f, 0.5f, 1.0f ),
                            "%s", status.c_str() );
    }
} // namespace Desert::Editor::Graph

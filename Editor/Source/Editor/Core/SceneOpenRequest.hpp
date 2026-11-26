#pragma once

#include <optional>
#include <string>

namespace Desert::Editor::Core
{
    // "Open this .desce" as a request, from anywhere, by path — the wire between a panel that received a
    // scene (the viewport's drag-drop target, the outliner) and EditorLayer, which is the only place that
    // may actually swap the document: a load tears down GPU resources and must happen BETWEEN frames,
    // never inside the ImGui call that noticed the drop. Same shape as [[PanelRequests]]: the requester
    // needs neither EditorLayer's type nor its instance.
    //
    // The editor consumes at most one request per frame; a second drop in the same frame replaces the
    // first (two scenes cannot both become the document anyway).
    class SceneOpenRequest
    {
    public:
        static void Request( const std::string& path )
        {
            Pending() = path;
        }

        // The pending path, if any; clears it.
        static std::optional<std::string> Consume()
        {
            if ( !Pending().has_value() )
                return std::nullopt;

            std::optional<std::string> path;
            path.swap( Pending() );
            return path;
        }

    private:
        static std::optional<std::string>& Pending();
    };
} // namespace Desert::Editor::Core

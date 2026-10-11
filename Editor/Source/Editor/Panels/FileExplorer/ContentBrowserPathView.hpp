#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct DirectoryInformation;
    class ContentDirectoryModel;

    /// THE CONTENT BROWSER'S LEFT PANE (UE: SPathView, with the favourites section above it): the pinned
    /// folders of the open project, then the project's folder tree. A click opens a folder; every folder node
    /// is a move-drop target for a dragged asset (and the asset view's folder tiles reuse the same target).
    /// It owns no navigation and no selection — the panel does, through the delegates.
    class ContentBrowserPathView
    {
    public:
        struct Delegates
        {
            std::function<void( DirectoryInformation* )> OnFolderSelected;    // a tree node was clicked
            std::function<void( const std::string& )>    OnFavouriteSelected; // a pinned folder was clicked
            std::function<std::vector<std::string>()>    SelectedPaths; // what a drag of a selected tile carries
            std::function<void()>                        OnMoved;       // a drop moved at least one asset
        };

        explicit ContentBrowserPathView( Delegates delegates );

        /// The pane as a child window @p width wide.
        void Draw( const ContentDirectoryModel& model, const DirectoryInformation* current, float width );

        /// The asset move-drop on the last item drawn, which IS the folder @p targetFolder (a tree node, a
        /// folder tile): any browser payload moves the dragged asset - or the selection it belongs to - into
        /// it through ContentBrowserUtils::MoveFileTo (registry rows leave a redirector, one undo step each).
        void AcceptMoveDropOnLastItem( const std::string& targetFolder ) const;

    private:
        void DrawFolder( const ContentDirectoryModel& model, const DirectoryInformation* current,
                         DirectoryInformation* dirInfo, bool defaultOpen = false );

        Delegates m_On;
    };
} // namespace Desert::Editor

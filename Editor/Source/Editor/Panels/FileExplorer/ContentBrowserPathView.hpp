#pragma once

#include <functional>
#include <string>

namespace Desert::Editor
{
    struct DirectoryInformation;
    class ContentDirectoryModel;

    /// THE CONTENT BROWSER'S LEFT PANE (UE: SPathView, with the favourites section above it): the pinned
    /// folders of the open project, then the project's folder tree. A click opens a folder; the pane is a
    /// move-drop target for a dragged asset. It owns no navigation — the panel does, through the delegates.
    class ContentBrowserPathView
    {
    public:
        struct Delegates
        {
            std::function<void( DirectoryInformation* )> OnFolderSelected;    // a tree node was clicked
            std::function<void( const std::string& )>    OnFavouriteSelected; // a pinned folder was clicked
        };

        explicit ContentBrowserPathView( Delegates delegates );

        /// The pane as a child window @p width wide, then the move-drop target on it.
        void Draw( const ContentDirectoryModel& model, const DirectoryInformation* current, float width );

        /// The asset move-drop on the last item drawn (the tree's child window, the right pane): a "selectable"
        /// payload moves its file into the folder hovered during the drag.
        void AcceptMoveDropOnLastItem();

    private:
        void DrawFolder( const ContentDirectoryModel& model, const DirectoryInformation* current,
                         DirectoryInformation* dirInfo, bool defaultOpen = false );

        Delegates   m_On;
        std::string m_MovePath;           // the folder hovered while an asset is dragged
        bool        m_IsDragging = false; // an asset drag is over the tree
    };
} // namespace Desert::Editor

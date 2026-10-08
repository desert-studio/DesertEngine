#pragma once

#include <Editor/Panels/FileExplorer/ContentBrowserCommands.hpp>
#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct DirectoryInformation;
    class ContentBrowserSelection;
    class ThumbnailEditMode;

    /// WHAT A RIGHT CLICK ON AN ASSET OFFERS, AND THE FILE OPERATIONS BEHIND IT (UE: FAssetContextMenu +
    /// SRenameWindow / the delete dialog). The item menu's action rows are ContentBrowserCommands drawn through
    /// their own info (label, shortcut) and run by Run — the one body the palette's commands reach too. The
    /// rename and delete-with-referrers modals, cut/copy/paste of the selection and the keyboard shortcuts on
    /// it (F2, Del, Ctrl/Cmd + C / X / V) live here. Every action is on ContentBrowserSelection.
    class AssetContextMenu
    {
    public:
        struct Delegates
        {
            std::function<void()>                        OnRefresh;     // re-list the open folder
            std::function<void( DirectoryInformation* )> OnOpenFolder;  // Open on one folder
            std::function<void( const std::string& )>    OnStatus;      // the red line (a failed file op)
            std::function<bool()>                        CanAddToScene; // a prefab's "Add to Scene"
            std::function<void( const std::string& )>    OnAddToScene;
        };

        AssetContextMenu( ContentBrowserSelection& selection, ThumbnailEditMode& thumbnailEdit,
                          Delegates delegates );

        /// The right-click menu on @p entry of @p folder (the item just drawn).
        void Draw( DirectoryInformation& entry, const DirectoryInformation* folder );
        /// The rename / delete modals, once per frame.
        void DrawPopups();
        /// F2 / Del / Ctrl(Cmd)+C X V on the selection, while the browser has focus and no text field does.
        void HandleShortcuts( const DirectoryInformation* folder );

        /// Opens the rename dialog on the current entry; refused, by name, when nothing is selected.
        Common::BoolResultStr Rename();
        /// Pastes the clipboard into @p folder (a cut moves, a copy copies).
        void Paste( const DirectoryInformation* folder );
        /// One ContentBrowserCommand on the selection in @p folder. Refused, with the reason, when the selection
        /// does not fit the command.
        Common::BoolResultStr Run( ContentBrowserCommand command, const DirectoryInformation* folder );

    private:
        // One menu row for @p command: its label and shortcut from the command's info, Run on click, a refusal
        // logged by name.
        void CommandMenuItem( ContentBrowserCommand command, const DirectoryInformation* folder,
                              bool selected = false, bool enabled = true );
        // Opens the delete confirmation on @p paths; @p scanReferencers lists who still points at them.
        void RequestDelete( std::vector<std::string> paths, bool scanReferencers );

        ContentBrowserSelection& m_Selection;
        ThumbnailEditMode&       m_ThumbnailEdit;
        Delegates                m_On;

        bool                     m_ShowRenamePopup = false;
        std::string              m_RenamePath;
        char                     m_RenameBuf[128] = { 0 };
        std::vector<std::string> m_RenameReferrers; // registry keys that keep loading through the redirector
        bool                     m_ShowDeleteConfirm = false;
        std::vector<std::string> m_PendingDeleteList; // paths queued for the delete-confirm modal
        std::vector<std::string> m_DeleteReferencers; // assets still pointing at the delete target(s)
    };
} // namespace Desert::Editor

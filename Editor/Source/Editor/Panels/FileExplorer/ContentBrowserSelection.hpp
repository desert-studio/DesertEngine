#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Editor
{
    struct AssetViewState;
    struct DirectoryInformation;

    /// THE CONTENT BROWSER'S SELECTION AND ITS CLIPBOARD (UE: SAssetView's selection set + the browser's
    /// copy/cut list behind FAssetContextMenu). Paths are the identity — a rescan frees every entry, so the one
    /// raw pointer kept, the "current" entry F2 renames, is released before a rescan and re-bound after it.
    class ContentBrowserSelection
    {
    public:
        [[nodiscard]] bool Contains( const DirectoryInformation* entry ) const;
        // The selected paths; the current entry alone when the set is empty.
        [[nodiscard]] std::vector<std::string> Paths() const;
        // The entries of @p folder that are selected, in the folder's order.
        [[nodiscard]] std::vector<DirectoryInformation*> EntriesIn( const DirectoryInformation* folder ) const;
        [[nodiscard]] DirectoryInformation*              Current() const
        {
            return m_Current;
        }

        // Plain / Ctrl(Cmd)-toggle / Shift-range click on the entry at display position @p shownIndex of
        // @p folder (the range walks the asset view's order).
        void Click( DirectoryInformation& entry, int shownIndex, const DirectoryInformation& folder,
                    const AssetViewState& view, bool showHidden );
        // @p entry becomes the whole selection (a right click outside it, Sync to Asset, a palette select).
        void SelectOnly( DirectoryInformation& entry );
        // A right click inside the selection: the selection stays, the clicked entry becomes current.
        void SetCurrent( DirectoryInformation& entry )
        {
            m_Current = &entry;
        }
        // An empty click / a delete: nothing selected (the range anchor stays).
        void Deselect();
        // The Clear Selection command: nothing selected, no range anchor.
        void Clear();

        // Before a rescan of the open folder: the current entry's path, the pointer dropped.
        [[nodiscard]] std::string ReleaseCurrent();
        // After the rescan: the entry the released path names now (null when it is gone).
        void Rebind( DirectoryInformation* entry )
        {
            m_Current = entry;
        }

        // Copy (@p cut false) or cut the selection.
        void                                          Copy( bool cut );
        [[nodiscard]] const std::vector<std::string>& Clipboard() const
        {
            return m_Clipboard;
        }
        [[nodiscard]] bool ClipboardIsCut() const
        {
            return m_ClipboardCut;
        }
        // A paste happened: a cut is consumed by it, a copy can be pasted again.
        void Pasted();

    private:
        std::unordered_set<std::string> m_Paths;
        DirectoryInformation*           m_Current     = nullptr;
        int                             m_AnchorShown = -1; // display index of the Shift-range anchor
        std::vector<std::string>        m_Clipboard;        // cut/copied paths
        bool                            m_ClipboardCut = false;
    };
} // namespace Desert::Editor

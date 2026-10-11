#pragma once

// THE SELECTION AND THE ROWS OF A LIST VIEW (UE STableViewBase / ITypedTableView, ListView.h:72 SelectionMode,
// TileView.h:15, TreeView.h:20) — the part of a list that is a model rather than a drawing.
//
// Pure: nothing here draws, reads input or touches a view. The walk (Widgets/ScrollList.cpp) and the
// enumeration (UICanvasLayout.cpp) both ask these functions, so a row that is selected, expanded or indented
// is the same row for the click and for the picture.

#include <UI/Args/UIScrollArgs.hpp>
#include <UI/UITree.hpp>
#include <Common/Core/KeyCodes.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::UI
{
    class UICollection;
    class UIDataStore;

    // WHICH ITEM A ROW IS, across inserts and removes above it — UE's "item object" identity. Never 0 (0 is
    // "none"). A child row is its entity; a collection row is (collection serial, record id), because a tree
    // shows records of many collections and record ids are only unique inside one.
    using UIListItemKey = std::uint64_t;

    [[nodiscard]] UIListItemKey ChildItemKey( NodeId child );
    [[nodiscard]] UIListItemKey RecordItemKey( const UICollection& owner, int index );

    // key(i) for i in [0, count): the one question selection needs of a list's rows.
    using UIListKeyAt = std::function<UIListItemKey( int )>;

    // THE SELECTION OF ONE LIST (per canvas cell, like the list's binding bookkeeping): the selected items,
    // the anchor a Shift range starts from (UE RangeSelectionStart) and the keyboard cursor (UE SelectorItem).
    struct UIListSelection
    {
        std::unordered_set<UIListItemKey> Selected;
        UIListItemKey                     Anchor = 0;
        UIListItemKey                     Cursor = 0;

        [[nodiscard]] bool Has( UIListItemKey key ) const
        {
            return Selected.contains( key );
        }
    };

    // Index of @p key among the @p count rows, or -1. Linear: asked on an event (a Shift click, a key), never
    // per row per frame.
    [[nodiscard]] int IndexOfKey( int count, const UIListKeyAt& keyAt, UIListItemKey key );

    // A pointer press on row @p index with these modifiers (STableViewBase::Private_OnItemClicked): Single
    // selects it; SingleToggle selects it or clears it when it already was; Multi adds Ctrl as a toggle and
    // Shift as the range from the anchor (Ctrl+Shift adds the range to what is selected). True when the set
    // of selected items changed.
    bool ClickRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt, int index,
                   bool shift, bool ctrl );

    // The keyboard cursor moved to row @p index (STableViewBase::NavigateToItem + KeyboardSelect): the item
    // under it becomes the selection; in Multi, Shift extends the range from the anchor and Ctrl moves the
    // cursor without selecting. True when the set of selected items changed.
    bool NavigateRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt, int index,
                      bool shift, bool ctrl );

    // Space on the cursor: Multi and SingleToggle toggle it, Single selects it.
    bool ToggleCursorRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt );

    // Ctrl+A in Multi. Everything else refuses (false).
    bool SelectAllRows( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt );

    // Drop the items @p alive says are gone, and the anchor / cursor with them. True when the selected set
    // shrank.
    bool PruneSelection( UIListSelection& sel, const std::function<bool( UIListItemKey )>& alive );

    // Where a navigation key moves the cursor in a list of @p count items laid out @p columns per line, with
    // @p pageItems items per screenful (STableViewBase::OnKeyDown). Up/Down move a line, Left/Right an item in
    // a TILE view only (columns > 1), Home/End to the ends, PageUp/PageDown a screenful. -1 when the key is
    // not a navigation key here; @p cursor (may be -1: no cursor yet) when it is but cannot move.
    [[nodiscard]] int NavigateTarget( Common::KeyCode key, int cursor, int count, int columns, int pageItems );

    // --- THE TREE'S ROWS (UE STreeView: the linearised, expanded part of the tree) -------------------------
    //
    // A tree row is a record of a collection; its children are the records of the collection whose KEY its
    // ChildrenField names (UITreeViewData). Only an EXPANDED row's children are asked for, so the rows are
    // produced by walking exactly the visible structure: a collapsed subtree of a million records costs one
    // field lookup, which is the point of TreeView (UE STreeView::Private_PopulateLinearizedItems descends
    // only into expanded items).
    struct UITreeRow
    {
        const UICollection* Owner       = nullptr;
        int                 Index       = 0; // record index in Owner
        int                 Depth       = 0;
        int                 Parent      = -1; // row index of the parent row, -1 for a root
        bool                HasChildren = false;
        bool                Expanded    = false;
        UIListItemKey       Key         = 0;
    };

    // What a flattening read, so a cached flattening can tell whether it still describes the data.
    struct UITreeSeen
    {
        std::string   Key;
        std::uint64_t Serial     = 0;
        std::uint64_t Generation = 0;
    };

    // The child collection of record @p record under @p childrenField, from @p store (null when the field is
    // empty or names no collection).
    [[nodiscard]] const UICollection* TreeChildren( const UIDataStore& store, const UIDataStore& record,
                                                 const std::string& childrenField );

    // Flatten @p roots (key @p rootsKey in @p store) into @p out, descending into rows whose key is in
    // @p expanded. @p seen receives every collection read (null = do not record).
    void FlattenTree( const UIDataStore& store, const std::string& rootsKey, const std::string& childrenField,
                      const std::unordered_set<UIListItemKey>& expanded, std::vector<UITreeRow>& out,
                      std::vector<UITreeSeen>* seen );

    // Does @p seen still describe @p store? (Every collection a flattening read is the same one at the same
    // generation.)
    [[nodiscard]] bool TreeSeenCurrent( const UIDataStore& store, const std::vector<UITreeSeen>& seen );

    // --- ONE LIST AS A VIEW SEES IT, AND ITS ROWS ---------------------------------------------------------

    // What a list remembers between frames as a VIEW of it rather than as the scene's data (one per list per
    // canvas cell, UICanvasContext::Lists): which rows are selected (UE keeps it in the widget, never in the
    // asset), which tree rows are expanded, and the tree's linearised rows — rebuilt only when the expansion
    // or a collection the last build read has changed, so a frame of an unchanged tree costs one lookup per
    // expanded row and not a walk.
    struct UIListViewState
    {
        UIListSelection                   Selection;
        std::unordered_set<UIListItemKey> Expanded;
        std::vector<UITreeRow>            TreeRows;
        std::vector<UITreeSeen>           TreeSeen;
        std::string                       TreeRoots; // the collection + field the rows were built from
        std::string                       TreeField;
        bool                              TreeStale = true;
    };

    // Bring @p state's tree rows up to date with @p store (no-op while they still describe it).
    void RefreshTreeRows( UIListViewState& state, const UIDataStore& store, const std::string& rootsKey,
                          const std::string& childrenField );

    // THE ROWS OF ONE LIST ELEMENT, resolved once per walk by both walks (renderer and enumeration): how many,
    // laid out how (tile columns), and what each row is (a child entity, a bound record, a tree record).
    struct UIListShape
    {
        int                   Count     = 0;
        int                   Columns   = 1;
        float                 ItemWidth = 0.0f;      // design px; 0 = full-width rows
        const UICollection*   Bound     = nullptr;   // a flat collection-bound list
        const UITreeViewData* Tree      = nullptr;   // a tree view (bound); rows are Rows()
        const std::vector<UITreeRow>* Cached = nullptr;
        std::vector<UITreeRow>        Own;

        [[nodiscard]] const std::vector<UITreeRow>& Rows() const
        {
            return Cached != nullptr ? *Cached : Own;
        }
        // The record row @p i answers its bindings from (null for a child row).
        [[nodiscard]] const UIDataStore* RowRecord( int i ) const;
        [[nodiscard]] UIListItemKey      KeyAt( const IUITree& tree, NodeId list, int i ) const;
    };

    // @p viewportWPx is the list's width in pixels (tile columns); @p state is the view's state of this list,
    // or null for a query with no view (no expansion, nothing cached).
    [[nodiscard]] UIListShape ResolveListShape( const IUITree& tree, NodeId e, float viewportWPx, float scale,
                                                const UIListViewState* state );
} // namespace Desert::UI

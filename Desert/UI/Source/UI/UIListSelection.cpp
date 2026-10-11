#include <UI/UIListSelection.hpp>

#include <UI/UIDataStore.hpp>
#include <UI/UILayout.hpp>

#include <algorithm>

namespace Desert::UI
{
    UIListItemKey ChildItemKey( NodeId child )
    {
        return static_cast<UIListItemKey>( static_cast<std::uint32_t>( child ) ) + 1u;
    }

    UIListItemKey RecordItemKey( const UICollection& owner, int index )
    {
        // Serial and record id are both counters from 1, so neither half reaches 2^32 in a session.
        return ( owner.Serial() << 32u ) ^ owner.RecordId( index );
    }

    int IndexOfKey( int count, const UIListKeyAt& keyAt, UIListItemKey key )
    {
        if ( key == 0 )
            return -1;
        for ( int i = 0; i < count; ++i )
            if ( keyAt( i ) == key )
                return i;
        return -1;
    }

    namespace
    {
        // Replace the selection with [a, b] (inclusive, either order), or add it when @p add.
        void SelectRange( UIListSelection& sel, const UIListKeyAt& keyAt, int a, int b, bool add )
        {
            if ( !add )
                sel.Selected.clear();
            for ( int i = std::min( a, b ); i <= std::max( a, b ); ++i )
                sel.Selected.insert( keyAt( i ) );
        }

        bool SelectOnly( UIListSelection& sel, UIListItemKey key )
        {
            const bool same = sel.Selected.size() == 1 && sel.Selected.contains( key );
            sel.Selected.clear();
            sel.Selected.insert( key );
            return !same;
        }
    } // namespace

    bool ClickRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt, int index,
                   bool shift, bool ctrl )
    {
        if ( mode == UISelectionMode::None || index < 0 || index >= count )
            return false;
        const UIListItemKey key = keyAt( index );
        sel.Cursor              = key;
        if ( mode == UISelectionMode::Single )
        {
            sel.Anchor = key;
            return SelectOnly( sel, key );
        }
        if ( mode == UISelectionMode::SingleToggle )
        {
            sel.Anchor = key;
            if ( sel.Has( key ) )
            {
                sel.Selected.clear();
                return true;
            }
            return SelectOnly( sel, key );
        }
        // Multi.
        if ( shift )
        {
            const auto before = sel.Selected;
            int        anchor = IndexOfKey( count, keyAt, sel.Anchor );
            if ( anchor < 0 )
            {
                anchor     = index;
                sel.Anchor = key;
            }
            SelectRange( sel, keyAt, anchor, index, ctrl );
            return before != sel.Selected;
        }
        sel.Anchor = key;
        if ( ctrl )
        {
            if ( !sel.Selected.erase( key ) )
                sel.Selected.insert( key );
            return true;
        }
        return SelectOnly( sel, key );
    }

    bool NavigateRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt, int index,
                      bool shift, bool ctrl )
    {
        if ( index < 0 || index >= count )
            return false;
        const UIListItemKey key = keyAt( index );
        sel.Cursor              = key;
        if ( mode == UISelectionMode::None )
            return false;
        if ( mode == UISelectionMode::Multi )
        {
            if ( ctrl )
                return false; // the cursor moves alone; Space then toggles what it is on
            if ( shift )
            {
                const auto before = sel.Selected;
                int        anchor = IndexOfKey( count, keyAt, sel.Anchor );
                if ( anchor < 0 )
                {
                    anchor     = index;
                    sel.Anchor = key;
                }
                SelectRange( sel, keyAt, anchor, index, false );
                return before != sel.Selected;
            }
        }
        sel.Anchor = key;
        return SelectOnly( sel, key );
    }

    bool ToggleCursorRow( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt )
    {
        const int index = IndexOfKey( count, keyAt, sel.Cursor );
        if ( mode == UISelectionMode::None || index < 0 )
            return false;
        if ( mode == UISelectionMode::Single )
            return SelectOnly( sel, sel.Cursor );
        sel.Anchor = sel.Cursor;
        if ( sel.Selected.erase( sel.Cursor ) )
            return true;
        if ( mode == UISelectionMode::SingleToggle )
            sel.Selected.clear();
        sel.Selected.insert( sel.Cursor );
        return true;
    }

    bool SelectAllRows( UIListSelection& sel, UISelectionMode mode, int count, const UIListKeyAt& keyAt )
    {
        if ( mode != UISelectionMode::Multi || count <= 0 )
            return false;
        const std::size_t before = sel.Selected.size();
        SelectRange( sel, keyAt, 0, count - 1, false );
        return before != sel.Selected.size();
    }

    bool PruneSelection( UIListSelection& sel, const std::function<bool( UIListItemKey )>& alive )
    {
        const std::size_t erased =
             std::erase_if( sel.Selected, [&alive]( UIListItemKey k ) { return !alive( k ); } );
        if ( sel.Anchor != 0 && !alive( sel.Anchor ) )
            sel.Anchor = 0;
        if ( sel.Cursor != 0 && !alive( sel.Cursor ) )
            sel.Cursor = 0;
        return erased != 0;
    }

    int NavigateTarget( Common::KeyCode key, int cursor, int count, int columns, int pageItems )
    {
        using Common::KeyCode;
        if ( count <= 0 )
            return -1;
        const int cols = std::max( 1, columns );
        const int page = std::max( cols, pageItems );
        // No cursor yet: the first navigation key lands on the first row, as a fresh STableViewBase does.
        if ( cursor < 0 || cursor >= count )
        {
            switch ( key )
            {
                case KeyCode::Up:
                case KeyCode::Down:
                case KeyCode::Home:
                case KeyCode::PageUp:
                case KeyCode::PageDown:
                    return 0;
                case KeyCode::End:
                    return count - 1;
                case KeyCode::Left:
                case KeyCode::Right:
                    return cols > 1 ? 0 : -1;
                default:
                    return -1;
            }
        }
        switch ( key )
        {
            case KeyCode::Up:
                return cursor - cols >= 0 ? cursor - cols : cursor;
            case KeyCode::Down:
                return cursor + cols < count ? cursor + cols : cursor;
            case KeyCode::Left:
                return cols > 1 ? std::max( 0, cursor - 1 ) : -1;
            case KeyCode::Right:
                return cols > 1 ? std::min( count - 1, cursor + 1 ) : -1;
            case KeyCode::Home:
                return 0;
            case KeyCode::End:
                return count - 1;
            case KeyCode::PageUp:
                return std::max( cursor % cols, cursor - page );
            case KeyCode::PageDown:
                return cursor + page < count ? cursor + page : cursor + ( ( count - 1 - cursor ) / cols ) * cols;
            default:
                return -1;
        }
    }

    const UICollection* TreeChildren( const UIDataStore& store, const UIDataStore& record,
                                      const std::string& childrenField )
    {
        if ( childrenField.empty() )
            return nullptr;
        const auto key = record.Text( childrenField );
        return key.has_value() && !key->empty() ? store.FindCollection( *key ) : nullptr;
    }

    void FlattenTree( const UIDataStore& store, const std::string& rootsKey, const std::string& childrenField,
                      const std::unordered_set<UIListItemKey>& expanded, std::vector<UITreeRow>& out,
                      std::vector<UITreeSeen>* seen )
    {
        out.clear();
        if ( seen != nullptr )
            seen->clear();
        const UICollection* roots = store.FindCollection( rootsKey );
        if ( seen != nullptr )
            seen->push_back( { rootsKey, roots != nullptr ? roots->Serial() : 0,
                               roots != nullptr ? roots->Generation() : 0 } );
        if ( roots == nullptr )
            return;

        // Pre-order, iteratively: a frame per collection being walked. A collection already on the path is
        // not entered again, so a record naming its own collection (or an ancestor's) as its children shows
        // as a leaf instead of an endless tree.
        struct Frame
        {
            const UICollection* Coll;
            int                 Next;
            int                 ParentRow;
        };
        std::vector<Frame> stack{ { roots, 0, -1 } };
        while ( !stack.empty() )
        {
            Frame& f = stack.back();
            if ( f.Next >= f.Coll->Size() )
            {
                stack.pop_back();
                continue;
            }
            const int i = f.Next++;

            UITreeRow row;
            row.Owner                    = f.Coll;
            row.Index                    = i;
            row.Depth                    = static_cast<int>( stack.size() ) - 1;
            row.Parent                   = f.ParentRow;
            row.Key                      = RecordItemKey( *f.Coll, i );
            const auto          childKey = f.Coll->Record( i ).Text( childrenField );
            const UICollection* children = TreeChildren( store, f.Coll->Record( i ), childrenField );
            const bool          onPath =
                 children != nullptr && std::any_of( stack.begin(), stack.end(),
                                                     [children]( const Frame& s ) { return s.Coll == children; } );
            row.HasChildren = children != nullptr && children->Size() > 0 && !onPath;
            row.Expanded    = row.HasChildren && expanded.contains( row.Key );
            out.push_back( row );
            if ( row.Expanded )
            {
                if ( seen != nullptr )
                    seen->push_back( { *childKey, children->Serial(), children->Generation() } );
                stack.push_back( { children, 0, static_cast<int>( out.size() ) - 1 } );
            }
        }
    }

    bool TreeSeenCurrent( const UIDataStore& store, const std::vector<UITreeSeen>& seen )
    {
        if ( seen.empty() )
            return false;
        for ( const UITreeSeen& s : seen )
        {
            const UICollection* c = store.FindCollection( s.Key );
            if ( ( c != nullptr ? c->Serial() : 0 ) != s.Serial ||
                 ( c != nullptr ? c->Generation() : 0 ) != s.Generation )
                return false;
        }
        return true;
    }

    void RefreshTreeRows( UIListViewState& state, const UIDataStore& store, const std::string& rootsKey,
                          const std::string& childrenField )
    {
        if ( !state.TreeStale && state.TreeRoots == rootsKey && state.TreeField == childrenField &&
             TreeSeenCurrent( store, state.TreeSeen ) )
            return;
        FlattenTree( store, rootsKey, childrenField, state.Expanded, state.TreeRows, &state.TreeSeen );
        state.TreeRoots = rootsKey;
        state.TreeField = childrenField;
        state.TreeStale = false;
    }

    const UIDataStore* UIListShape::RowRecord( int i ) const
    {
        if ( Tree != nullptr )
        {
            const UITreeRow& r = Rows()[static_cast<std::size_t>( i )];
            return &r.Owner->Record( r.Index );
        }
        return Bound != nullptr ? &Bound->Record( i ) : nullptr;
    }

    UIListItemKey UIListShape::KeyAt( const IUITree& tree, NodeId list, int i ) const
    {
        if ( Tree != nullptr )
            return Rows()[static_cast<std::size_t>( i )].Key;
        if ( Bound != nullptr )
            return RecordItemKey( *Bound, i );
        return ChildItemKey( tree.ChildAt( list, static_cast<std::size_t>( i ) ) );
    }

    UIListShape ResolveListShape( const IUITree& tree, NodeId e, float viewportWPx, float scale,
                                  const UIListViewState* state )
    {
        UIListShape shape;
        const auto* lv = tree.Get<UIListViewData>( e );
        if ( lv == nullptr )
            return shape;
        const UIDataStore& store = UIDataStore::Get();
        if ( tree.Has<UITileViewData>( e ) )
        {
            const auto& tile = *tree.Get<UITileViewData>( e );
            shape.ItemWidth  = std::max( 1.0f, tile.ItemWidth );
            shape.Columns   = TileColumns( shape.ItemWidth, lv->Spacing, viewportWPx, scale );
        }
        if ( lv->Collection.empty() )
        {
            shape.Count = static_cast<int>( tree.ChildCount( e ) );
            return shape;
        }
        // A tree is a bound list whose rows are the linearised expanded records; the tile grid does not apply
        // to it (a tree's rows are indented lines, UE's STreeView is a list, never a tile view).
        if ( tree.Has<UITreeViewData>( e ) )
        {
            const auto& tv  = *tree.Get<UITreeViewData>( e );
            shape.Tree      = &tv;
            shape.Columns   = 1;
            shape.ItemWidth = 0.0f;
            if ( state != nullptr && !state->TreeStale && state->TreeRoots == lv->Collection &&
                 state->TreeField == tv.ChildrenField && TreeSeenCurrent( store, state->TreeSeen ) )
                shape.Cached = &state->TreeRows;
            else
            {
                static const std::unordered_set<UIListItemKey> kNone;
                FlattenTree( store, lv->Collection, tv.ChildrenField, state != nullptr ? state->Expanded : kNone,
                             shape.Own, nullptr );
            }
            shape.Count = static_cast<int>( shape.Rows().size() );
            return shape;
        }
        shape.Bound = store.FindCollection( lv->Collection );
        shape.Count = shape.Bound != nullptr ? shape.Bound->Size() : 0;
        return shape;
    }
} // namespace Desert::UI

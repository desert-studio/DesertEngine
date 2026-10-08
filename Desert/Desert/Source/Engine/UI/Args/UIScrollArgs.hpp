#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>
#include <glm/glm.hpp>

// Scrolling containers: the free scroll view and the virtualised list view.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // A vertical scroll view: clips its children to its rect and scrolls them by the mouse wheel. Children are
    // positioned relative to the content top (offset up by ScrollY). ContentHeight is the total scrollable
    // height in design px (author-set); scrolling clamps to [0, ContentHeight - viewport height].
    struct UIScrollViewData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::ScrollView;

        PROPERTY( DisplayName( "Scroll Y" ), Category( "UI Scroll View" ) )
        float ScrollY = 0.0f;

        PROPERTY( DisplayName( "Content Height" ), Category( "UI Scroll View" ) )
        float ContentHeight = 600.0f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Scroll View" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Show Scrollbar" ), Category( "UI Scroll View" ) )
        bool ShowScrollbar = true;

        PROPERTY( DisplayName( "Scrollbar Color" ), Category( "UI Scroll View" ), Color )
        glm::vec3 ScrollbarColor = glm::vec3( 0.35f, 0.37f, 0.44f );
    };

    // A VERTICAL LIST WHOSE OFF-SCREEN ROWS DO NOT EXIST FOR THE FRAME (Ю17).
    //
    // WHY IT IS A SECOND CONTAINER AND NOT A FLAG ON UIScrollView. A scroll view holds ARBITRARY content:
    // its children keep their own anchors, it cannot know where any of them ends up without resolving all
    // of them, and resolving all of them is the O(n) it would have to avoid. This one owns its rows'
    // geometry the way UILayoutGroup owns its children's — every row is ItemHeight design px tall and
    // Spacing apart — which is exactly what turns "which rows are on screen" into two divisions. The two
    // are different contracts, not two modes of one, and a `Virtualize` bool on the scroll view would have
    // meant a container that positions its children on Tuesdays.
    //
    // ROW INDEX IS CHILD ORDER, AND VISIBILITY DOES NOT MOVE IT. A Hidden or Collapsed child leaves an
    // empty row rather than closing the gap, which is the one place this container disagrees with
    // UILayoutGroup. Closing the gap needs a running count over every child ahead of the window — the
    // whole-list pass this element exists to delete — so it would cost precisely what it saves. The row is
    // still skipped by the walk's own visibility axis, so the hole is visible and is not a silent draw.
    //
    // CONTENT HEIGHT IS DERIVED, NEVER AUTHORED. UIScrollViewData carries a ContentHeight the author has
    // to keep in step with what is actually in the list; here it is the child count times the pitch, so a
    // row added or removed cannot leave the scroll range lying about it.
    struct UIListViewData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::ListView;

        PROPERTY( DisplayName( "Scroll Y" ), Category( "UI List View" ) )
        float ScrollY = 0.0f;

        // The pitch's first half, and the reason the window is arithmetic instead of a search. Clamped to
        // at least 1 design px where it is read: a pitch of zero makes the window unbounded, which turns
        // this element into the whole-list walk it exists to replace.
        PROPERTY( DisplayName( "Item Height" ), Category( "UI List View" ), Range( 1.0f, 512.0f ) )
        float ItemHeight = 40.0f;

        PROPERTY( DisplayName( "Spacing" ), Category( "UI List View" ), Range( 0.0f, 128.0f ) )
        float Spacing = 0.0f;

        // Rows kept in the walk past each edge of the viewport. NOT a safety margin for sloppy geometry —
        // it is what gives a row's per-frame state a life beyond one pixel of scrolling. A UIRenderTexture
        // row holds a renderer slot that comes back by DESTRUCTION (UIRenderTextureSource.hpp), so at zero
        // overscan a one-pixel jitter across the edge tears down a whole SceneRenderer and builds it again
        // next frame. One row of overscan is what that costs to avoid.
        PROPERTY( DisplayName( "Overscan Rows" ), Category( "UI List View" ), Range( 0, 8 ) )
        int Overscan = 1;

        // The two colours are the SCROLLING-CONTAINER slots, shared with UIScrollView on purpose — see
        // UIStyleSlots.hpp. They must stay equal to UIScrollViewData's, or a theme that binds the one slot
        // changes this element's look and not that one's; Desert/Tests/Engine/UIListView pins it.
        PROPERTY( DisplayName( "Background" ), Category( "UI List View" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Show Scrollbar" ), Category( "UI List View" ) )
        bool ShowScrollbar = true;

        PROPERTY( DisplayName( "Scrollbar Color" ), Category( "UI List View" ), Color )
        glm::vec3 ScrollbarColor = glm::vec3( 0.35f, 0.37f, 0.44f );

        // BOUND TO DATA (UIL1) — the UMG ListView + entry-widget pattern. Empty: rows are this element's
        // children, as above. Set: rows are the records of this UIDataStore collection, and the element's
        // ONE child is the entry template, drawn once per visible record with that record's fields
        // answering its UIBinding keys first (a field named like a global key shadows it inside the row).
        // Nothing is instantiated per record, so ten thousand records cost the window and not the list.
        PROPERTY(
             DisplayName( "Collection" ), Category( "UI List View" ),
             Tooltip( "Data-store collection key (Lua ui.list_add). Rows are its records; the one child is the "
                      "entry template" ) )
        std::string Collection;

        // A chat log's behaviour: while the list is scrolled to its end, records appended to the collection
        // keep it at the end. Scrolled up, the reader's place is kept instead — which is also what every
        // bound list does for records inserted or removed ABOVE its window.
        PROPERTY( DisplayName( "Follow End" ), Category( "UI List View" ),
                  Tooltip( "Stay scrolled to the end while at the end when records are added (chat)" ) )
        bool FollowEnd = false;
    };
} // namespace Desert::UI

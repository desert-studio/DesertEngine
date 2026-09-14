// "Every UI component type the editor can create, the renderer can draw."
//
// The defect this exists to prevent, measured on 2026-09-05: the UI Editor panel's create menu offered ten
// element types while the renderer that panel previewed with knew six of the engine's twenty-two. Pressing
// "+ Slider" in that window produced a slider the same window could not show. Both ends looked right — the
// menu created a real component, the shipping renderer drew it in the viewport — and the middle link, the
// panel's private renderer, silently lost six of them.
//
// A comment saying "these N types are covered" is exactly what went stale there, so the statement is a test
// instead, and it MEASURES both sides rather than asserting a declaration:
//
//   * the editor side is Editor/Panels/UI/UIElementCatalog.hpp, the one list both create menus are generated
//     from (there used to be two lists, and they had already drifted);
//   * the renderer side is read out of Engine/UI/UICanvasRenderer2D.cpp's OWN DISPATCH — the `has<ECS::T>` /
//     `view<ECS::T>` calls it makes on the registry. Not a header, not a table somebody maintains beside the
//     code: if a case is deleted, the query goes with it and this test goes red. A mention in a comment is
//     not enough to pass, because a comment does not query the registry.
//
// And it is bidirectional, in the SettingConsumers shape: every UI component the renderer handles must
// appear in exactly one row here — a catalog entry, or an explicit exclusion WITH ITS REASON. A twenty-third
// component fails this test until somebody decides which of the two it is. That decision is the point.

#include <Editor/Panels/UI/UIElementCatalog.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Editor::kUIElementCount;
using Desert::Editor::kUIElements;

namespace
{
    // THE WALK IS TWO FILES SINCE Ю12, and this census has to read both or it stops being complete.
    // UIOverlay.cpp is not a second renderer: it is the part of the same frame that decides which overlay
    // canvases are open and where they are, called by EndUIFrame and by nothing else. A component queried
    // only there — the overlay trigger is — is handled by the shipping UI exactly as much as one queried
    // in the draw walk, and reading one file would have called it unhandled.
    constexpr const char* kRendererFiles[] = {
         "Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp",
         "Desert/Desert/Source/Engine/UI/UIOverlay.cpp",
    };
    constexpr const char* kRenderer = "the UI walk (UICanvasRenderer2D.cpp + UIOverlay.cpp)";

    // Component types the shipping renderer handles that are deliberately NOT offered by the create menus.
    // Each needs a reason, and the reason is the row.
    struct Exclusion
    {
        const char* Type;
        const char* Why;
    };

    constexpr Exclusion kExclusions[] = {
         // The named action moved when the UI editor became a document (U7-2): its "Create UI Canvas"
         // button was an empty state INSIDE the window, and a document is opened over a canvas that
         // exists. The two doors that were always there are the ones left.
         { "UICanvasComponent", "the canvas root itself — created by the viewport toolbar's UI ▸ UI Canvas "
                                "or Details ▸ Add Component ▸ UI Canvas, and a second canvas would not be "
                                "drawn (the renderer takes the first)" },
         { "UILayoutComponent", "the rect. Every element gets one automatically in AddUIChild; on its own "
                                "it is an invisible box" },
         { "UIScreenComponent", "the screen machine: a screen with no name is skipped by the renderer's "
                                "seeding loop, so a menu-created one would be invisible" },
         { "UIScreenStackComponent", "belongs on the canvas entity, not on a child element" },
         // TRUE SINCE U7-2 AND NOT BEFORE IT. This row said "added in Details" while the only control that
         // added one was a button inside the Sequencer panel; the timeline is a document over this
         // component now, so a window cannot exist without it and the button moved to Details ▸ UI Layout
         // ▸ "Add UI Animation", where the thing is actually made.
         { "UIAnimComponent", "modifier — a keyed clip added in Details to an element that already exists" },
         { "UIBindingComponent", "modifier — binds a data-store value into an element that already exists" },
         { "UITweenComponent", "modifier — a from/to animation on an element that already exists" },
         { "UIDraggableComponent", "modifier — makes an existing element draggable" },
         { "UIDropTargetComponent", "modifier — makes an existing element a drop target" },
         { "UIPointerEventsComponent", "modifier — adds enter/exit/press messages to an existing element" },
         // Overlays (Ю12). Both belong on something that already exists — an overlay on a CANVAS, a
         // trigger on an element — so neither is an entry in a menu that creates children of a canvas.
         // The overlay's own door is the viewport's UI ▸ Overlay submenu, which creates a canvas with the
         // component and a working example of its chrome (CreateUIOverlay).
         { "UIOverlayComponent", "belongs on a CANVAS entity, not on a child element — created with its "
                                 "chrome by the viewport toolbar's UI ▸ Overlay submenu" },
         { "UIOverlayTriggerComponent", "modifier — makes an existing element open an overlay on hover or "
                                        "on a click" },
    };

    // The repository root, found by walking up from wherever the test binary was started — the same approach
    // the setting-consumers and font-baker tests use, so none of them has to be run from one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    constexpr const char* kFactory = "Editor/Source/Editor/Panels/UI/UIElementFactory.hpp";

    // The two menus that create UI. Neither may build a UI entity of its own — see the creation-is-a-
    // command test at the bottom of this file.
    constexpr const char* kUIMenus[] = {
         "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp",
         "Editor/Source/Editor/Panels/UI/UIEditorPanel.cpp",
    };

    // Every source file of the UI walk, concatenated. Concatenated rather than scanned one at a time
    // because the question the census asks — "does the shipping UI ever query this component" — is about
    // the walk and not about which of its files the query happens to sit in.
    std::string WalkSource( const std::string& root )
    {
        std::string all;
        for ( const char* file : kRendererFiles )
            all += ReadFile( root + file ) + "\n";
        return all;
    }

    // Every UI component type the renderer actually QUERIES: the `has<ECS::UIxxx>` / `view<ECS::UIxxx>` calls
    // in its source. This is the census, read from the dispatch itself rather than from any list.
    std::set<std::string> RendererDispatch( const std::string& source )
    {
        std::set<std::string> types;
        // The three ways this renderer reaches a component. `get<` also covers `try_get<`, so the census
        // survives a refactor from has+get to try_get; measured 2026-09-05, all three agree on 22 types.
        for ( const char* call : { "has<ECS::UI", "get<ECS::UI", "view<ECS::UI" } )
        {
            const std::string needle( call );
            for ( std::size_t at = source.find( needle ); at != std::string::npos;
                  at             = source.find( needle, at + 1 ) )
            {
                const std::size_t nameStart = at + needle.size() - 2; // keep "UI"
                const std::size_t close     = source.find( '>', nameStart );
                if ( close == std::string::npos )
                    continue;
                types.insert( source.substr( nameStart, close - nameStart ) );
            }
        }
        return types;
    }
} // namespace

// Every element the editor can create is a type the shipping renderer dispatches on. This is the direction
// that was broken: ten creatable types, six drawable ones.
TEST( UIElementCensus, EveryCreatableElementIsDrawnByTheRenderer )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string source = WalkSource( root );
    ASSERT_FALSE( source.empty() ) << kRenderer << " could not be read";

    const std::set<std::string> dispatch = RendererDispatch( source );
    ASSERT_FALSE( dispatch.empty() ) << "read " << kRenderer << " but found no ECS::UI* dispatch in it — the "
                                     << "renderer was restructured and this test no longer measures anything";

    for ( std::size_t i = 0; i < kUIElementCount; ++i )
    {
        const std::string type = kUIElements[i].ComponentType;
        EXPECT_TRUE( dispatch.count( type ) == 1 )
             << "the editor's create menus offer '" << kUIElements[i].Label << "' (ECS::" << type << ") but "
             << kRenderer << " never asks the registry for it — the element would be created "
             << "and never drawn. Add a case to the renderer, or drop the catalog entry.";
    }
}

// ...and the other direction: nothing the renderer handles is left undecided. A component added to the
// renderer must become a catalog entry or an exclusion with a reason.
TEST( UIElementCensus, EveryDrawnComponentIsEitherCreatableOrExcludedWithAReason )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source = WalkSource( root );
    ASSERT_FALSE( source.empty() ) << kRenderer << " could not be read";

    std::set<std::string> accounted;
    for ( std::size_t i = 0; i < kUIElementCount; ++i )
        accounted.insert( kUIElements[i].ComponentType );
    for ( const Exclusion& x : kExclusions )
    {
        EXPECT_TRUE( accounted.insert( x.Type ).second )
             << x.Type << " is both a catalog entry and an exclusion — it must be exactly one";
        EXPECT_GT( std::string( x.Why ).size(), 20u ) << x.Type << " is excluded without a reason";
    }

    for ( const std::string& type : RendererDispatch( source ) )
        EXPECT_TRUE( accounted.count( type ) == 1 )
             << "ECS::" << type << " is drawn by " << kRenderer
             << " but is neither an entry in UIElementCatalog.hpp nor an exclusion in this test. Decide "
             << "which it is: an element the author can place, or something attached to one.";
}

// The two lists partition the renderer's census exactly — no entry and no exclusion names a component the
// renderer does not handle at all. Without this a stale row could hide a deleted case.
TEST( UIElementCensus, NoCatalogEntryOrExclusionIsAGhost )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::set<std::string> dispatch = RendererDispatch( WalkSource( root ) );

    for ( const Exclusion& x : kExclusions )
        EXPECT_TRUE( dispatch.count( x.Type ) == 1 )
             << "ECS::" << x.Type << " is excluded from the create menus here, but the renderer does not "
             << "handle it either — the row is stale and should go.";

    // Every UI component the ENGINE has is one the renderer handles, so the census is complete: 12 creatable
    // + 10 excluded. The number is pinned so that growing either side is a deliberate edit of this file.
    EXPECT_EQ( dispatch.size(), kUIElementCount + sizeof( kExclusions ) / sizeof( kExclusions[0] ) );
}

// The loop closes at the ENGINE: a UI component that exists at all must be one the renderer draws and one
// this file has placed. Without this the two tests above would happily agree about a set that had quietly
// stopped being all of them — a twenty-third component could be declared, serialized, given a Details
// section and never drawn by anything, which is the very shape the panel's dead preview had.
TEST( UIElementCensus, EveryUIComponentTheEngineDeclaresIsHandledAndPlaced )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string components = ReadFile( root + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
    ASSERT_FALSE( components.empty() ) << "Engine/ECS/Components.hpp could not be read";

    // Declared UI component structs. `*Data` payloads and the UI enums are not components and do not match.
    std::set<std::string> declared;
    const std::string     decl( "struct UI" );
    for ( std::size_t at = components.find( decl ); at != std::string::npos; at = components.find( decl, at + 1 ) )
    {
        const std::size_t nameStart = at + decl.size() - 2; // keep "UI"
        std::size_t       end       = nameStart;
        while ( end < components.size() &&
                ( std::isalnum( static_cast<unsigned char>( components[end] ) ) || components[end] == '_' ) )
            ++end;
        const std::string name = components.substr( nameStart, end - nameStart );
        if ( name.find( "Component" ) != std::string::npos )
            declared.insert( name );
    }
    ASSERT_GE( declared.size(), 20u ) << "found only " << declared.size()
                                      << " UI component declarations — the scan no longer measures anything";

    const std::set<std::string> dispatch = RendererDispatch( WalkSource( root ) );

    std::set<std::string> accounted;
    for ( std::size_t i = 0; i < kUIElementCount; ++i )
        accounted.insert( kUIElements[i].ComponentType );
    for ( const Exclusion& x : kExclusions )
        accounted.insert( x.Type );

    for ( const std::string& name : declared )
    {
        EXPECT_TRUE( dispatch.count( name ) == 1 )
             << "ECS::" << name << " is declared in Components.hpp but " << kRenderer
             << " never asks the registry for it: nothing in the engine draws it.";
        EXPECT_TRUE( accounted.count( name ) == 1 )
             << "ECS::" << name << " is declared in Components.hpp but is neither an entry in "
             << "UIElementCatalog.hpp nor an exclusion in this test.";
    }
}

// --- CREATING A UI ELEMENT IS AN UNDOABLE ACTION -------------------------------------------------
//
// Every other creator in the editor — the outliner's Add menu, the prefab drop, the viewport mesh drop,
// the file explorer's instantiate — calls Commands::NotifyCreated, so Ctrl+Z takes the entity back. The
// two UI menus were the only ones that did not: an element added to a canvas could be removed only by
// finding it in the outliner and deleting it by hand, and the canvas itself — the very first thing a
// user creates when authoring UI — the same.
//
// Read as TEXT, and from the factory rather than from the menus, because that is the claim: the record
// happens in the ONE place an element is made, so a third UI menu cannot be written without it. The
// two menus already drifted apart once when each carried its own AddUIChild, which is why the factory
// exists at all.
TEST( UIElementCensus, EveryUICreationPathRecordsItselfOnTheUndoStack )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string factory = ReadFile( root + kFactory );
    ASSERT_FALSE( factory.empty() ) << "cannot read " << kFactory;

    EXPECT_NE( factory.find( "Commands::NotifyCreated" ), std::string::npos )
         << kFactory << " never records a creation, so a UI element cannot be undone";

    // The element creator: the catalog macro must expand through the recording helper, not straight to
    // AddUIChild. This is the line that would go back to being an unrecorded creation.
    EXPECT_NE( factory.find( "RecordUICreation( scene, AddUIChild<ECS::Type>" ), std::string::npos )
         << "CreateUIElement builds the entity without recording it";

    // And the canvas, which is a different door and had the same gap.
    EXPECT_NE( factory.find( "CreateUICanvas" ), std::string::npos )
         << "there is no recorded way to create the canvas itself";

    for ( const char* menu : kUIMenus )
    {
        const std::string source = ReadFile( root + menu );
        ASSERT_FALSE( source.empty() ) << "cannot read " << menu;
        EXPECT_EQ( source.find( "AddComponent<ECS::UICanvasComponent>" ), std::string::npos )
             << menu
             << " builds a UI canvas inline instead of calling CreateUICanvas, so that creation "
                "is not on the undo stack";
        EXPECT_EQ( source.find( "AddUIChild<" ), std::string::npos )
             << menu << " builds a UI element inline instead of calling CreateUIElement";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

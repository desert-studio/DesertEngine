// Closing a scene view: its view must go away, and the SURVIVORS must still be themselves.
//
// This file sits in the ViewLifetime suite rather than a new one because the two halves are one fact. A
// scene view is a view with a window around it: opening one creates a view, closing one must destroy it, and the
// reason closing was not implemented for so long is that doing it naively breaks the OTHER half — every surviving
// viewport's activation callback used to capture its document's POSITION, and a position survives a removal in the
// worst possible way. It stays in range, it still resolves, and it now names a different document. Asserting the
// view count alone would have passed a build in which clicking the third viewport bound the editor to the second
// one's scene.
//
// So the model below is the editor's own bookkeeping, driven through the REAL types:
// Engine/Graphic/ViewResources.hpp for the views and Editor/Core/SceneViewIdentity.hpp for the naming.
// Neither needs a Vulkan device, which is the whole point — EditorLayer.cpp is one of the 47 of 48 editor
// panel/layer translation units that NO suite compiles (scripts/CI/UnreachedSources.sh), so anything that
// is going to be assertable at all has to be lifted out of it into a header like these two.

#include <Editor/Core/SceneViewIdentity.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using Desert::Editor::ActiveSceneViewAfterClose;
using Desert::Editor::IndexOfSceneView;
using Desert::Editor::kPrimarySceneViewId;
using Desert::Editor::SceneViewIdSource;
using Desert::Graphic::ViewResourceRegistry;
using Desert::Graphic::ViewResources;

namespace
{
    // EditorLayer::SceneDocument, minus everything that needs a GPU. Its ViewResources is the one the
    // SceneRenderer owns, so destroying a document here ends a view for exactly the same reason it does in
    // the editor.
    struct Document
    {
        Document( uint64_t id, std::string name ) : Id( id ), Name( std::move( name ) ), View( Name )
        {
        }

        uint64_t      Id;
        std::string   Name;
        ViewResources View;
    };

    // EditorLayer's multi-document bookkeeping: AddSceneView / CloseSceneView / SetActiveScene, with the
    // same containers, the same id source and the same two pure functions the shipping code calls. What is
    // deliberately NOT re-implemented here is any of the naming logic — a copy of the rules could drift from
    // the rules, and a test that passes against a copy is worth nothing.
    class Editor
    {
    public:
        // The primary viewport: always open, holds a view for the whole session, cannot be closed.
        Editor() : m_Primary( "Scene 1" )
        {
        }

        uint64_t AddSceneView()
        {
            const uint64_t id = m_Ids.Next();
            m_Docs.push_back( std::make_unique<Document>( id, "Scene " + std::to_string( id + 1 ) ) );
            return id;
        }

        // What EditorLayer::CloseSceneView does, in the same order: rebind the active document if it was
        // this one, then destroy. Returns false for an id that is already gone.
        bool CloseSceneView( uint64_t id )
        {
            const auto index = IndexOf( id );
            if ( !index )
                return false;

            m_ActiveId = ActiveSceneViewAfterClose( m_ActiveId, id );
            m_Docs.erase( m_Docs.begin() + static_cast<ptrdiff_t>( *index ) );
            return true;
        }

        // What a viewport's activation callback does when its window takes focus. It is handed the ID its
        // document was created with — never a position — and this is the function the "close the middle one"
        // test is really about.
        void Activate( uint64_t id )
        {
            if ( id != kPrimarySceneViewId && !IndexOf( id ) )
            {
                m_LastRefusedId = id; // the editor logs and leaves the active document alone
                return;
            }
            m_ActiveId = id;
        }

        [[nodiscard]] uint64_t ActiveId() const
        {
            return m_ActiveId;
        }

        // The name of the document the editor is bound to — the observable the user actually judges by,
        // since it is what the Outliner, Details, gizmo and Play button follow.
        [[nodiscard]] std::string ActiveName() const
        {
            if ( m_ActiveId == kPrimarySceneViewId )
                return "Scene 1";
            const auto index = IndexOf( m_ActiveId );
            return index ? m_Docs[*index]->Name : std::string( "<none>" );
        }

        [[nodiscard]] std::optional<size_t> IndexOf( uint64_t id ) const
        {
            return IndexOfSceneView( m_Docs, []( const std::unique_ptr<Document>& doc ) { return doc->Id; }, id );
        }

        [[nodiscard]] size_t OpenViewCount() const
        {
            return m_Docs.size();
        }

        [[nodiscard]] uint64_t LastRefusedId() const
        {
            return m_LastRefusedId;
        }

    private:
        ViewResources                          m_Primary;
        SceneViewIdSource                      m_Ids;
        std::vector<std::unique_ptr<Document>> m_Docs;
        uint64_t                               m_ActiveId      = kPrimarySceneViewId;
        uint64_t                               m_LastRefusedId = kPrimarySceneViewId;
    };
} // namespace

// --- The requirement, stated as the lead stated it ---------------------------------------------------

TEST( SceneViewLifetime, ClosingTheMiddleViewLeavesTheThirdActivatingTheThird )
{
    // Three views open; close the middle; focus the third. The third must become active.
    //
    // Under the position-capturing callback this is the exact case that failed silently: document 3 was
    // created third, so its callback held index 2. Closing document 2 slid document 3 down to index 1 and
    // left index 2 out of range — or, with a fourth view open, pointing at a stranger. Neither is a crash.
    Editor editor;

    const uint64_t first  = editor.AddSceneView();
    const uint64_t second = editor.AddSceneView();
    const uint64_t third  = editor.AddSceneView();

    ASSERT_EQ( editor.IndexOf( third ), std::optional<size_t>( 2 ) );

    ASSERT_TRUE( editor.CloseSceneView( second ) );

    // The trap, asserted rather than described: the third document has MOVED, so any callback holding its
    // old position now names the document that used to be behind it.
    EXPECT_EQ( editor.IndexOf( third ), std::optional<size_t>( 1 ) );
    EXPECT_EQ( editor.IndexOf( second ), std::nullopt );
    EXPECT_EQ( editor.IndexOf( first ), std::optional<size_t>( 0 ) );

    editor.Activate( third );
    EXPECT_EQ( editor.ActiveId(), third );
    EXPECT_EQ( editor.ActiveName(), "Scene 4" ) << "focusing the third view activated a different document";

    // And the first one still activates the first — a fix that made every callback resolve to the LAST
    // document would satisfy the assertion above on its own.
    editor.Activate( first );
    EXPECT_EQ( editor.ActiveName(), "Scene 2" );
}

TEST( SceneViewLifetime, ClosingTheMiddleViewEndsExactlyOneView )
{
    // The same sequence, counted instead of named. One primary + three views = four views; closing one must
    // return one, not zero and not two.
    Editor editor;
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u );

    editor.AddSceneView();
    const uint64_t second = editor.AddSceneView();
    editor.AddSceneView();
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 4u );

    ASSERT_TRUE( editor.CloseSceneView( second ) );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 3u );
    EXPECT_EQ( editor.OpenViewCount(), 2u );
}

// --- Identity ----------------------------------------------------------------------------------------

TEST( SceneViewLifetime, IdsAreNeverReused )
{
    // The property the whole design rests on. If a closed view's id were handed to a later view, a stale
    // callback would find a LIVE document under its old name and activate a stranger — the dangling index
    // again, in different clothes, and this time undetectable by the empty-optional check.
    Editor                editor;
    std::vector<uint64_t> everIssued;

    for ( int cycle = 0; cycle < 8; ++cycle )
    {
        const uint64_t id = editor.AddSceneView();
        EXPECT_EQ( std::count( everIssued.begin(), everIssued.end(), id ), 0 )
             << "id " << id << " was issued twice (cycle " << cycle << ")";
        everIssued.push_back( id );
        EXPECT_NE( id, kPrimarySceneViewId ) << "an extra view was named with the primary's reserved id";
        ASSERT_TRUE( editor.CloseSceneView( id ) );
    }
}

TEST( SceneViewLifetime, AStaleIdActivatesNothingAndSaysSo )
{
    // The failure an id has and a position does not: it ANSWERS. A viewport that outlived its document
    // resolves to nothing, the active document is left alone, and the editor has something to log.
    Editor editor;

    const uint64_t first  = editor.AddSceneView();
    const uint64_t second = editor.AddSceneView();
    editor.Activate( first );

    ASSERT_TRUE( editor.CloseSceneView( second ) );
    editor.Activate( second );

    EXPECT_EQ( editor.ActiveId(), first ) << "a stale id moved the active document";
    EXPECT_EQ( editor.LastRefusedId(), second );
}

TEST( SceneViewLifetime, ClosingTheSameViewTwiceIsHarmless )
{
    // Two closes can reach CloseSceneView for one document — the window's X and the Scenes menu item both
    // clear the same visibility flag, and a close is deferred to the top of the next frame.
    Editor editor;

    const uint64_t only = editor.AddSceneView();
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 2u );

    EXPECT_TRUE( editor.CloseSceneView( only ) );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u );
    EXPECT_FALSE( editor.CloseSceneView( only ) );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u ) << "the second close ended a view that was not ours";
}

// --- Where the editor is left pointing ---------------------------------------------------------------

TEST( SceneViewLifetime, ClosingTheACTIVEViewFallsBackToThePrimary )
{
    // The primary is the only document guaranteed to exist, so it is the only safe landing place. Leaving
    // m_MainScene pointing at a destroyed scene is what every panel would then draw from.
    Editor editor;

    const uint64_t view = editor.AddSceneView();
    editor.Activate( view );
    ASSERT_EQ( editor.ActiveId(), view );

    ASSERT_TRUE( editor.CloseSceneView( view ) );
    EXPECT_EQ( editor.ActiveId(), kPrimarySceneViewId );
    EXPECT_EQ( editor.ActiveName(), "Scene 1" );
}

TEST( SceneViewLifetime, ClosingAnInactiveViewDoesNotMoveTheUser )
{
    // Tidying up a spare viewport is not a request to be teleported into another scene. This is the clause
    // that a "just reset to the primary on any close" implementation would break.
    Editor editor;

    const uint64_t working = editor.AddSceneView();
    const uint64_t spare   = editor.AddSceneView();
    editor.Activate( working );

    ASSERT_TRUE( editor.CloseSceneView( spare ) );
    EXPECT_EQ( editor.ActiveId(), working );
    EXPECT_EQ( editor.ActiveName(), "Scene 2" );
}

// --- The measurement the lead asked for --------------------------------------------------------------

TEST( SceneViewLifetime, OpeningAndClosingViewsForeverLeavesOnlyThePrimary )
{
    // The leak's shape: a closed scene view that keeps its view costs nothing visible until the byte
    // budget refuses a window the user opens later. Twenty cycles, and the count is back every time.
    Editor         editor;
    const uint32_t baseline = ViewResourceRegistry::LiveCount();
    ASSERT_EQ( baseline, 1u );

    for ( int cycle = 0; cycle < 20; ++cycle )
    {
        const uint64_t id = editor.AddSceneView();
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), baseline + 1 ) << "cycle " << cycle;
        ASSERT_TRUE( editor.CloseSceneView( id ) );
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), baseline )
             << "cycle " << cycle << ": a closed scene view kept its view";
    }
    EXPECT_EQ( editor.OpenViewCount(), 0u );
}

TEST( SceneViewLifetime, TwelveViewsAreAllLiveAtOnceAndAllComeBack )
{
    // Past the six the retired slot pool allowed: every view is its own, none folds onto the primary's.
    Editor editor;

    std::vector<uint64_t> ids;
    for ( uint32_t i = 0; i < 11; ++i )
        ids.push_back( editor.AddSceneView() );

    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 12u );

    // Closed newest-first, which is neither the order they were opened nor the order the vector holds them
    // after any of the erases above.
    for ( auto it = ids.rbegin(); it != ids.rend(); ++it )
        ASSERT_TRUE( editor.CloseSceneView( *it ) );

    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u ) << "only the primary viewport should still hold a view";
}

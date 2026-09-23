// ── A COMMAND MUST LAND ON THE VIEWPORT THE USER IS IN ─────────────────────────────────────────────
//
// Several viewports are live at once here: that is a shipped feature (Scenes -> New Scene View, and the
// four-up grid opens three more panes on one scene). So "Viewport Camera: Top" from the palette, and the
// same request over the control channel, have to pick one — and `ViewportPanel::ActiveViewport()` picked
// it by asking who holds the BONE-AUTHORING context, falling back to the first live viewport.
//
// THOSE ARE TWO DIFFERENT QUESTIONS. The authoring context is legitimately held by a Sequencer document
// or by the Details bone tree, neither of which is a viewport, and in that state the answer collapsed to
// "the first one in the list" with focus never consulted at all. Measured consequence: "Viewport Camera:
// Top" re-aimed a grid pane while the user was driving the main Scene pane.
//
// The editor already knew the answer — twice. Multi-scene activation follows viewport focus and so does
// bone authoring. A THIRD copy of the same state is the defect and not the fix, so the rule under test
// STORES NOTHING: it reads ImGui's own focus order, which ImGui maintains whether anybody looks or not.
//
// WHAT EACH ROW IS FOR:
//
//   * `TheMostRecentlyFocusedViewportWins` is the claim, and it is the row the old implementation fails:
//     with the Scene pane focused last it returned the first pane in the list instead.
//   * `AWindowThatIsNotAViewportDoesNotStealTheAnswer` is the one that makes it usable at all. The
//     palette, the Outliner and the Details panel are ALL focused more recently than the viewport the
//     user came from — every single time a command is issued — so a rule that read only the last entry
//     would answer "no viewport" for every command the editor has.
//   * `AViewportThatIsGoneIsNotChosen` is the negative control on the order: it carries windows that no
//     longer exist as panels, which is what a closed viewport's entry looks like.
//   * `NothingHasEverBeenFocused` and `NoViewportsAtAll` are the two cold states, and they are different
//     answers: the first live one, and nothing.

#include <Editor/Panels/ViewportPanel/ActiveViewportRule.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using Desert::Editor::ActiveViewportIndex;
using Desert::Editor::WindowNamesPanel;

namespace
{
    // WHAT IMGUI ACTUALLY HOLDS IN `ImGuiWindow::Name`, and it is not the panel's name.
    // `EditorLayer::PanelDisplayTitle` builds "<icon>  <label>###<GetName()>" and hands THAT to Begin(),
    // so the identity is the tail after the last "###" (IPanel.hpp says why). Spelled here the same way
    // the editor spells it, because a test that matched on a simplified name would be testing a string
    // the editor never produces.
    std::string WindowNameFor( std::string_view panelName )
    {
        std::string label( panelName );
        if ( const auto pos = label.find( "###" ); pos != std::string::npos )
            label.erase( pos );
        return "\xef\x84\x9e  " + label + "###" + std::string( panelName );
    }

    // The four panes of the grid, named as EditorLayer names them: the primary Scene window and three
    // extra views keyed on their scene-view id.
    const std::string kScene = "Scene###scene";
    const std::string kPane2 = "New Scene (view 2)###sceneviewport1";
    const std::string kPane3 = "New Scene (view 3)###sceneviewport2";
    const std::string kPane4 = "New Scene (view 4)###sceneviewport3";

    const std::vector<std::string_view> kFourUp = { kScene, kPane2, kPane3, kPane4 };

    // THE WINDOW NAMES AS OBJECTS THAT OUTLIVE THE CALL. `ActiveViewportIndex` takes string_views, and a
    // vector of views built straight out of `WindowNameFor(...)` would point at temporaries that die at
    // the end of the full expression — a dangling read that happens to pass today and not tomorrow.
    const std::string kSceneWindow = WindowNameFor( kScene );
    const std::string kPane2Window = WindowNameFor( kPane2 );
    const std::string kPane3Window = WindowNameFor( kPane3 );
    const std::string kPane4Window = WindowNameFor( kPane4 );
} // namespace

TEST( ActiveViewport, AWindowIsMatchedByItsImGuiIdentityAndNotByItsLabel )
{
    // The decorated half is what the user reads and changes with the scene's name; the half after the
    // last "###" is what ImGui keys the window on and what never moves. Matching on the whole string
    // would break the moment a scene is renamed, which is a thing the user does.
    EXPECT_TRUE( WindowNamesPanel( WindowNameFor( kScene ), kScene ) );
    EXPECT_TRUE( WindowNamesPanel( WindowNameFor( kPane4 ), kPane4 ) );
    EXPECT_FALSE( WindowNamesPanel( WindowNameFor( kPane4 ), kPane3 ) );

    // A window whose name merely ENDS with the panel's text is not that panel: the separator has to be
    // there, or "###sceneviewport13" would answer for "###sceneviewport3".
    EXPECT_FALSE( WindowNamesPanel( "Details###details", kScene ) );
    EXPECT_FALSE( WindowNamesPanel( "x" + std::string( kScene ), kScene ) );
    EXPECT_FALSE( WindowNamesPanel( WindowNameFor( kScene ), "" ) );
}

TEST( ActiveViewport, TheMostRecentlyFocusedViewportWins )
{
    // THE ROW THE OLD IMPLEMENTATION FAILS. The grid opened and pane 4 came up last, then the user
    // clicked into the main Scene pane and drove it. The command means the Scene pane.
    const std::vector<std::string_view> order = { kPane2Window, kPane3Window, kPane4Window, kSceneWindow };
    const auto                          index = ActiveViewportIndex( kFourUp, order );
    ASSERT_TRUE( index.has_value() );
    EXPECT_EQ( *index, 0u );

    // And the other way round, so the row cannot be satisfied by an implementation that always answers
    // "the first one" — which is exactly what the old one did.
    const std::vector<std::string_view> reversed = { kSceneWindow, kPane2Window, kPane3Window, kPane4Window };
    const auto                          last     = ActiveViewportIndex( kFourUp, reversed );
    ASSERT_TRUE( last.has_value() );
    EXPECT_EQ( *last, 3u );
}

TEST( ActiveViewport, AWindowThatIsNotAViewportDoesNotStealTheAnswer )
{
    // EVERY command arrives in this state. The palette is a window and it has the focus at the moment
    // the command is issued; the Outliner and Details are focused whenever the user selects something
    // there. "Nothing is focused right now" must not mean "fall back to the first viewport" — the order
    // still remembers which viewport the user came from, which is the whole reason it is read instead of
    // a per-frame focus flag.
    const std::vector<std::string_view> order = {
         kSceneWindow, kPane4Window, "Scene Outliner###Scene Outliner", "Details###Details", "##CommandPalette",
    };
    const auto index = ActiveViewportIndex( kFourUp, order );
    ASSERT_TRUE( index.has_value() );
    EXPECT_EQ( *index, 3u ) << "pane 4 was the last viewport the user was in";
}

TEST( ActiveViewport, AViewportThatIsGoneIsNotChosen )
{
    // ImGui keeps a closed window in its order; the panel list does not. The answer must come from the
    // intersection, or a command would be aimed at a window that no longer exists — the same lifetime
    // trap `SceneViewIdentity.hpp` argues about indices.
    const std::vector<std::string_view> live  = { kScene, kPane2 };
    const std::vector<std::string_view> order = { kSceneWindow, kPane2Window, kPane4Window };
    const auto                          index = ActiveViewportIndex( live, order );
    ASSERT_TRUE( index.has_value() );
    EXPECT_EQ( *index, 1u ) << "pane 4 is in the focus order but is not a live viewport";
}

TEST( ActiveViewport, NothingHasEverBeenFocused )
{
    // The editor's cold start: the layout is built, the windows exist, the user has not clicked. The
    // first live viewport is the only defensible answer and it is the one the old code always gave.
    const auto index = ActiveViewportIndex( kFourUp, {} );
    ASSERT_TRUE( index.has_value() );
    EXPECT_EQ( *index, 0u );
}

TEST( ActiveViewport, NoViewportsAtAll )
{
    // A DIFFERENT ANSWER FROM "the first one", and the caller turns it into a named refusal rather than
    // a silent success — over the control channel those two are indistinguishable to the sender.
    const std::vector<std::string_view> order = { kSceneWindow };
    EXPECT_FALSE( ActiveViewportIndex( {}, order ).has_value() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

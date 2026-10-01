// PanelMaximize (CTL2): "Maximize panel: <name>" / "Restore panel" and the dock layout they return to.
#include <Editor/Core/PanelMaximize.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::PanelMaximize;
    using Step = PanelMaximize::Step;

    constexpr std::uint32_t kBottomDock = 0x51u;

    // What the layer hands Before(): the dock layout as it stands, counted so a test can say WHEN it is read.
    struct LayoutProbe
    {
        std::string                  Text  = "[Docking][Data]\nDockSpace ID=0x51";
        int                          Calls = 0;
        std::function<std::string()> Fn()
        {
            return [this]
            {
                ++Calls;
                return Text;
            };
        }
    };

    std::vector<std::string> Labels( PanelMaximize& state, const std::vector<std::string>& docked )
    {
        std::vector<std::string> labels;
        for ( const auto& command : Desert::Editor::PanelMaximizePaletteCommands( state, docked ) )
            labels.push_back( command.Group + "/" + command.Label );
        return labels;
    }

    Desert::Editor::PaletteCommand Find( PanelMaximize& state, const std::vector<std::string>& docked,
                                         const std::string& label )
    {
        for ( auto& command : Desert::Editor::PanelMaximizePaletteCommands( state, docked ) )
            if ( command.Label == label )
                return command;
        return {};
    }
} // namespace

TEST( PanelMaximize, OffersMaximizeForEveryDockedPanelAndNoRestore )
{
    PanelMaximize state;
    EXPECT_EQ( Labels( state, { "Assets", "Details" } ),
               ( std::vector<std::string>{ "Panel/Maximize panel: Assets", "Panel/Maximize panel: Details" } ) );
}

TEST( PanelMaximize, MaximizeUndocksOnceThenRestoreHandsBackTheLayoutCapturedBeforeUndocking )
{
    PanelMaximize state;
    LayoutProbe   layout;
    auto          maximize = Find( state, { "Assets" }, "Maximize panel: Assets" );
    ASSERT_TRUE( maximize.Run );
    ASSERT_TRUE( maximize.Run().IsSuccess() );

    // Another panel's Begin is untouched.
    EXPECT_EQ( state.Before( "Details", 0x77u, layout.Fn() ), Step::None );
    EXPECT_EQ( layout.Calls, 0 );

    // The layout is read on the undocking frame, BEFORE the panel leaves its node, and only then.
    EXPECT_EQ( state.Before( "Assets", kBottomDock, layout.Fn() ), Step::Undock );
    EXPECT_EQ( layout.Calls, 1 );
    EXPECT_EQ( state.MaximizedPanel(), "Assets" );
    const std::string captured = layout.Text;
    layout.Text                = "[Docking][Data]\nthe node Assets left is gone";

    // Floating from now on: nothing more to do each frame, and nothing to restore yet.
    EXPECT_EQ( state.Before( "Assets", 0, layout.Fn() ), Step::None );
    EXPECT_EQ( layout.Calls, 1 );
    EXPECT_FALSE( state.TakeLayoutToRestore().has_value() );
    EXPECT_EQ( Labels( state, {} ), ( std::vector<std::string>{ "Panel/Restore panel" } ) );

    // Restore hands back the CAPTURED layout whole — not a node id, which may have died with the undock.
    auto restore = Find( state, {}, "Restore panel" );
    ASSERT_TRUE( restore.Run().IsSuccess() );
    EXPECT_EQ( state.Before( "Assets", 0, layout.Fn() ), Step::None ); // pending restore survives the Begin
    const auto restored = state.TakeLayoutToRestore();
    ASSERT_TRUE( restored.has_value() );
    EXPECT_EQ( *restored, captured );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_FALSE( state.TakeLayoutToRestore().has_value() ); // once
    EXPECT_EQ( state.Before( "Assets", kBottomDock, layout.Fn() ), Step::None );
}

TEST( PanelMaximize, RestoreNeedsAMaximizedPanelAndOnlyOneIsMaximized )
{
    PanelMaximize state;
    const auto    noneMaximized = state.RequestRestore();
    ASSERT_FALSE( noneMaximized.IsSuccess() );

    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    (void)state.Before( "Assets", kBottomDock, LayoutProbe{}.Fn() );
    const auto second = state.RequestMaximize( "Logs" );
    ASSERT_FALSE( second.IsSuccess() );
}

TEST( PanelMaximize, DockingBackByHandCountsAsRestored )
{
    PanelMaximize state;
    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    (void)state.Before( "Assets", kBottomDock, LayoutProbe{}.Fn() );
    // The user dragged the tab into some dock: the saved node must not move it a second time.
    EXPECT_EQ( state.Before( "Assets", 0x99u, LayoutProbe{}.Fn() ), Step::None );
    EXPECT_FALSE( state.TakeLayoutToRestore().has_value() );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_EQ( Labels( state, { "Assets" } ), ( std::vector<std::string>{ "Panel/Maximize panel: Assets" } ) );
}

TEST( PanelMaximize, AFloatingPanelIsNotMaximized )
{
    PanelMaximize state;
    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    EXPECT_EQ( state.Before( "Assets", 0, LayoutProbe{}.Fn() ), Step::None );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_TRUE( state.RequestMaximize( "Logs" ).IsSuccess() );
}

TEST( PanelMaximize, QuittingWhileMaximizedKeepsTheLayoutFromBeforeTheMaximize )
{
    PanelMaximize state;
    LayoutProbe   layout;
    EXPECT_FALSE( state.LayoutToKeepOnQuit().has_value() ) << "nothing maximized: ImGui's own save stands";

    auto maximize = Find( state, { "Assets" }, "Maximize panel: Assets" );
    ASSERT_TRUE( maximize.Run );
    ASSERT_TRUE( maximize.Run().IsSuccess() );
    EXPECT_EQ( state.Before( "Assets", kBottomDock, layout.Fn() ), Step::Undock );
    const std::string captured = layout.Text;

    const auto kept = state.LayoutToKeepOnQuit();
    ASSERT_TRUE( kept.has_value() ) << "a maximized panel at quit would be saved as a floating window";
    EXPECT_EQ( *kept, captured );
}

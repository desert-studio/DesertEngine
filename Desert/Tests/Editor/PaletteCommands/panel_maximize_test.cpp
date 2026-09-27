// PanelMaximize (CTL2): "Maximize panel: <name>" / "Restore panel" and the dock node they return to.
#include <Editor/Core/PanelMaximize.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using Desert::Editor::PanelMaximize;
    using Step = PanelMaximize::Step;

    constexpr std::uint32_t kBottomDock = 0x51u;

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

TEST( PanelMaximize, MaximizeUndocksOnceThenRestoreRedocksIntoTheSavedNode )
{
    PanelMaximize state;
    auto          maximize = Find( state, { "Assets" }, "Maximize panel: Assets" );
    ASSERT_TRUE( maximize.Run );
    ASSERT_TRUE( maximize.Run().IsSuccess() );

    // Another panel's Begin is untouched.
    EXPECT_EQ( state.Before( "Details", 0x77u ).Kind, Step::None );

    const auto undock = state.Before( "Assets", kBottomDock );
    EXPECT_EQ( undock.Kind, Step::Undock );
    EXPECT_EQ( undock.DockId, 0u );
    EXPECT_EQ( state.MaximizedPanel(), "Assets" );

    // Floating from now on: nothing more to do each frame.
    EXPECT_EQ( state.Before( "Assets", 0 ).Kind, Step::None );
    EXPECT_EQ( Labels( state, {} ), ( std::vector<std::string>{ "Panel/Restore panel" } ) );

    auto restore = Find( state, {}, "Restore panel" );
    ASSERT_TRUE( restore.Run().IsSuccess() );
    const auto redock = state.Before( "Assets", 0 );
    EXPECT_EQ( redock.Kind, Step::Redock );
    EXPECT_EQ( redock.DockId, kBottomDock );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_EQ( state.Before( "Assets", kBottomDock ).Kind, Step::None );
}

TEST( PanelMaximize, RestoreNeedsAMaximizedPanelAndOnlyOneIsMaximized )
{
    PanelMaximize state;
    const auto    noneMaximized = state.RequestRestore();
    ASSERT_FALSE( noneMaximized.IsSuccess() );

    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    (void)state.Before( "Assets", kBottomDock );
    const auto second = state.RequestMaximize( "Logs" );
    ASSERT_FALSE( second.IsSuccess() );
}

TEST( PanelMaximize, DockingBackByHandCountsAsRestored )
{
    PanelMaximize state;
    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    (void)state.Before( "Assets", kBottomDock );
    // The user dragged the tab into some dock: the saved node must not move it a second time.
    EXPECT_EQ( state.Before( "Assets", 0x99u ).Kind, Step::None );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_EQ( Labels( state, { "Assets" } ), ( std::vector<std::string>{ "Panel/Maximize panel: Assets" } ) );
}

TEST( PanelMaximize, AFloatingPanelIsNotMaximized )
{
    PanelMaximize state;
    ASSERT_TRUE( state.RequestMaximize( "Assets" ).IsSuccess() );
    EXPECT_EQ( state.Before( "Assets", 0 ).Kind, Step::None );
    EXPECT_TRUE( state.MaximizedPanel().empty() );
    EXPECT_TRUE( state.RequestMaximize( "Logs" ).IsSuccess() );
}

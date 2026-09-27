// DetailsNavigation (CTL2): what the Details panel offers the palette, and the one-shot requests.
#include <Editor/Core/DetailsNavigation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::DetailsNavigation;

    constexpr std::uint64_t kPbr = 11;
    constexpr std::uint64_t kSky = 22;

    // One Details frame of an entity with a material slot and a transform section.
    void DrawPbrFrame( DetailsNavigation& nav )
    {
        nav.BeginFrame( kPbr, "PBR_Metal_0" );
        nav.NoteComponent( "Transform" );
        nav.NoteField( "Position" );
        nav.NoteComponent( "Materials" );
        nav.NotePicker( "Material slot 0" );
        nav.EndFrame();
    }

    std::vector<std::string> Labels( DetailsNavigation& nav )
    {
        std::vector<std::string> labels;
        for ( const auto& command : Desert::Editor::DetailsPaletteCommands( nav ) )
            labels.push_back( command.Group + "|" + command.Label );
        return labels;
    }

    bool Has( const std::vector<std::string>& labels, const std::string& label )
    {
        return std::find( labels.begin(), labels.end(), label ) != labels.end();
    }
} // namespace

TEST( DetailsNavigation, OffersOnlyWhatTheLastCompleteFrameDrew )
{
    DetailsNavigation nav;
    EXPECT_TRUE( Labels( nav ).empty() ) << "nothing drawn yet, nothing offered";

    DrawPbrFrame( nav );
    EXPECT_TRUE( Labels( nav ).empty() ) << "the frame being drawn is not published until the next BeginFrame";

    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    const auto labels = Labels( nav );
    EXPECT_TRUE( Has( labels, "Details|Show field: Transform" ) );
    EXPECT_TRUE( Has( labels, "Details|Show field: Transform / Position" ) );
    EXPECT_TRUE( Has( labels, "Details|Show field: Materials" ) );
    EXPECT_TRUE( Has( labels, "Details|Open picker: Material slot 0" ) );
    EXPECT_EQ( labels.size(), 4U );
}

TEST( DetailsNavigation, ChangingTheSelectionDropsTheOldEntitysFields )
{
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    nav.BeginFrame( kSky, "Sky" );
    EXPECT_TRUE( nav.Fields().empty() );
    EXPECT_TRUE( nav.Pickers().empty() );
}

TEST( DetailsNavigation, NotesOutsideADetailsFrameAreIgnored )
{
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    nav.NoteField( "Albedo" ); // the Material Editor draws reflected fields through the same builder
    nav.NotePicker( "Skybox" );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    EXPECT_EQ( nav.Fields().size(), 3U );
    EXPECT_EQ( nav.Pickers().size(), 1U );
}

TEST( DetailsNavigation, RequestIsTakenOnceAtItsDrawSite )
{
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    ASSERT_TRUE( nav.RequestPicker( "Material slot 0" ).IsSuccess() );
    using Step = DetailsNavigation::PickerStep;
    EXPECT_EQ( nav.TakePicker( "Skybox" ), Step::None );
    EXPECT_FALSE( nav.TakeReveal( "Material slot 0" ) ) << "a picker request is not a reveal";
    EXPECT_EQ( nav.TakePicker( "Material slot 0" ), Step::Scroll ) << "the first frame only scrolls the row in";
    EXPECT_EQ( nav.TakePicker( "Material slot 0" ), Step::Open ) << "the popup opens once the row is placed";
    EXPECT_EQ( nav.TakePicker( "Material slot 0" ), Step::None ) << "one-shot";
}

TEST( DetailsNavigation, RequestNotTakenByTheNextFrameExpires )
{
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    nav.EndFrame();
    // Delivered after this frame's draw: must survive into the next one.
    ASSERT_TRUE( nav.RequestReveal( "Transform / Position" ).IsSuccess() );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    nav.EndFrame(); // the section was collapsed: nobody took it
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    EXPECT_FALSE( nav.TakeReveal( "Transform / Position" ) ) << "a stale request fired a frame nobody asked about";
}

TEST( DetailsNavigation, RefusesWithTheReasonAndWhatIsShown )
{
    DetailsNavigation nav;
    auto              none = nav.RequestPicker( "Skybox" );
    ASSERT_FALSE( none.IsSuccess() );
    EXPECT_NE( none.GetError().find( "no entity is selected" ), std::string::npos ) << none.GetError();

    DrawPbrFrame( nav );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    auto missing = nav.RequestPicker( "Skybox" );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "'Skybox'" ), std::string::npos ) << missing.GetError();
    EXPECT_NE( missing.GetError().find( "PBR_Metal_0" ), std::string::npos ) << missing.GetError();
    EXPECT_NE( missing.GetError().find( "Material slot 0" ), std::string::npos ) << missing.GetError();
}

TEST( DetailsNavigation, PaletteEntryRechecksWhenRun )
{
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    const auto commands = Desert::Editor::DetailsPaletteCommands( nav );
    nav.EndFrame();
    nav.BeginFrame( kSky, "Sky" ); // selection moved between building the dictionary and running it
    for ( const auto& command : commands )
        EXPECT_FALSE( command.Run().IsSuccess() ) << command.Label;
}

// The live order: the command lands between two Details frames, the row scrolls in on the first, and the
// popup opens on the second — the expiry that drops an untaken request must not drop a scrolled one.
TEST( DetailsNavigation, PickerScrollsThenOpensOnTheNextFrame )
{
    using Step = DetailsNavigation::PickerStep;
    DetailsNavigation nav;
    DrawPbrFrame( nav );
    DrawPbrFrame( nav ); // the ledger a command reads is the one the PREVIOUS frame published
    ASSERT_TRUE( nav.RequestPicker( "Material slot 0" ).IsSuccess() );
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    EXPECT_EQ( nav.TakePicker( "Material slot 0" ), Step::Scroll );
    nav.EndFrame();
    nav.BeginFrame( kPbr, "PBR_Metal_0" );
    EXPECT_EQ( nav.TakePicker( "Material slot 0" ), Step::Open );
    nav.EndFrame();
}

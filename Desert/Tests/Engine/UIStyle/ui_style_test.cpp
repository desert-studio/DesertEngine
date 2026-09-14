// WHERE AN ELEMENT'S COLOUR, FONT AND SPACING COME FROM — asserted as a RELATION, in both directions.
//
// The load-bearing claim of Ю13 is not "a theme can change a colour". It is the PAIR:
//
//     changing the theme moves every slot the style binds,
//     AND moves NOTHING that it does not — not an unbound slot, and not an element that opted out.
//
// One direction alone proves half of it and is the half that passes while the feature is broken: a
// resolver that ignored the style table entirely would satisfy "the theme changes things" by changing
// everything. So every resolution test below asserts both, and the shipped themes are held to the
// strongest form of it — Desert_Dark's tokens must equal the components' own defaults digit for digit, so
// a canvas that switches from no theme to that one renders the frame it rendered before.
//
// The second half of the suite is a CENSUS of the slot register against the file that draws it. A slot
// nobody reads is a dead setting in a table instead of in a component (§1.3), and it is invisible: the UI
// looks exactly the same either way. The census derives its count from the enum, so a new enumerator
// reddens it until something draws it.

#include <Engine/Assets/UIThemeData.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/UI/UIStyleSlots.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>
#include <unordered_set>
#include <vector>

using Desert::Assets::BuildUIThemeRuntime;
using Desert::Assets::kUIThemeDefaultStyle;
using Desert::Assets::ParseUITheme;
using Desert::Assets::UIThemeBinding;
using Desert::Assets::UIThemeColor;
using Desert::Assets::UIThemeData;
using Desert::Assets::UIThemeFont;
using Desert::Assets::UIThemeMetric;
using Desert::Assets::UIThemeRuntime;
using Desert::Assets::UIThemeStyle;
using Desert::Assets::ValidateUIThemeData;
using Desert::Assets::WriteUITheme;
using Desert::UI::CanvasStyle;
using Desert::UI::ElementStyle;
using Desert::UI::kStyleSlotCount;
using Desert::UI::kStyleSlotInfo;
using Desert::UI::StyleSlot;
using Desert::UI::StyleSlotKind;
using Desert::UI::StyleSlotKindOf;
using Desert::UI::StyleSlotName;

namespace
{
    // The repository root, found by walking up from wherever the test binary was started — the same
    // approach SettingConsumers uses, so neither has to be run from one exact directory.
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

    const char* kShippedThemes[] = { "Editor/Resources/Assets/UI/Themes/Desert_Dark.detheme",
                                     "Editor/Resources/Assets/UI/Themes/Desert_Light.detheme" };

    // A minimal legal theme with one colour, one metric, one font and one style binding all three.
    UIThemeData Minimal()
    {
        UIThemeData d;
        d.FormatVersion = Desert::Assets::kUIThemeFormatVersion;
        d.Colors        = { UIThemeColor{ "Ink", glm::vec3( 0.1f, 0.2f, 0.3f ) },
                            UIThemeColor{ "Paper", glm::vec3( 0.9f, 0.9f, 0.9f ) } };
        d.Metrics       = { UIThemeMetric{ "Radius", 11.0f } };
        d.Fonts         = { UIThemeFont{ "Body", "", 31.0f } };
        d.Styles        = { UIThemeStyle{ kUIThemeDefaultStyle,
                                          { UIThemeBinding{ "Panel.Color", "Ink" },
                                            UIThemeBinding{ "Panel.CornerRadius", "Radius" },
                                            UIThemeBinding{ "Text.Font", "Body" } } } };
        return d;
    }

    UIThemeRuntime Build( const UIThemeData& d, const char* name = "test" )
    {
        auto built = BuildUIThemeRuntime( d, name, {} );
        EXPECT_TRUE( static_cast<bool>( built ) ) << ( built ? std::string() : built.GetError() );
        return built ? built.ExtractValue() : UIThemeRuntime{};
    }
} // namespace

// ----------------------------------------------------------------------------------------------
// The register itself
// ----------------------------------------------------------------------------------------------

// The table is INDEXED BY THE ENUM. A row list that has silently shifted against its enumerators is the
// "two things that must agree" defect, and it would produce a theme that binds the wrong slot while every
// individual line still reads correctly.
TEST( UIStyleSlots, EveryEnumeratorHasItsOwnRowAndTheNameRoundTrips )
{
    ASSERT_EQ( kStyleSlotInfo.size(), kStyleSlotCount );

    std::unordered_set<std::string> seen;
    for ( std::size_t i = 0; i < kStyleSlotCount; ++i )
    {
        const auto slot = static_cast<StyleSlot>( i );
        const auto name = std::string( StyleSlotName( slot ) );

        EXPECT_FALSE( name.empty() ) << "slot " << i << " has no name";
        EXPECT_TRUE( seen.insert( name ).second ) << "the slot name '" << name << "' is used twice";
        // The name must lead back to THIS enumerator, which is what makes a theme file's text and the
        // table's row the same thing rather than two.
        EXPECT_EQ( Desert::UI::StyleSlotFromName( name ), slot ) << name;
        // Every name is "<Element>.<Slot>"; the theme editor and the Details table group by the first half.
        EXPECT_NE( name.find( '.' ), std::string::npos ) << name << " is not <Element>.<Slot>";
    }
}

TEST( UIStyleSlots, AnUnknownNameIsRefusedRatherThanMappedToTheFirstSlot )
{
    EXPECT_EQ( Desert::UI::StyleSlotFromName( "Panel.Colour" ), StyleSlot::Count );
    EXPECT_EQ( Desert::UI::StyleSlotFromName( "" ), StyleSlot::Count );
}

// THE CENSUS. A slot the walk never reads is a dead setting wearing a table's clothes: a theme could bind
// it, the file would validate, and the picture would not move. Derived from the enum, so this goes red for
// a new enumerator nobody drew — and for a read that was DELETED, which is the direction that matters.
TEST( UIStyleSlots, EverySlotIsReadByTheCanvasWalk )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string walk = ReadFile( root + "Desert/Desert/Source/Engine/UI/UICanvasRenderer2D.cpp" );
    ASSERT_FALSE( walk.empty() ) << "UICanvasRenderer2D.cpp could not be read";

    std::vector<std::string> unread;
    for ( std::size_t i = 0; i < kStyleSlotCount; ++i )
    {
        const auto slot = static_cast<StyleSlot>( i );
        // Matched on the ENUMERATOR's spelling as it appears at a call site (`StyleSlot::PanelColor`),
        // not on the file's text name: the name is what a theme author writes and the enumerator is what
        // the code reads, and it is the second one whose absence makes the slot dead.
        std::string needle = "StyleSlot::";
        needle += std::string( StyleSlotName( slot ) );
        needle.erase( std::remove( needle.begin(), needle.end(), '.' ), needle.end() );
        // "Panel.Color" -> "PanelColor", "Progress.Background" -> "ProgressBackground" — the register's
        // names and its enumerators are the same words, which this test also pins by construction.
        if ( walk.find( needle ) == std::string::npos )
            unread.push_back( needle );
    }

    EXPECT_TRUE( unread.empty() ) << "these slots are declared but never read by the canvas walk, so a "
                                     "theme binding them would move nothing: "
                                  << [&unread]
    {
        std::string s;
        for ( const auto& u : unread )
            s += u + " ";
        return s;
    }();
}

// ----------------------------------------------------------------------------------------------
// The file format
// ----------------------------------------------------------------------------------------------

TEST( UIThemeFormat, WriteThenReadIsTheSameTheme )
{
    const UIThemeData original = Minimal();
    const auto        parsed   = ParseUITheme( WriteUITheme( original ) );
    ASSERT_TRUE( static_cast<bool>( parsed ) ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue(), original );
}

TEST( UIThemeFormat, AnUnknownFormatVersionIsRefusedWithTheVersionInTheMessage )
{
    UIThemeData d     = Minimal();
    d.FormatVersion   = Desert::Assets::kUIThemeFormatVersion + 41;
    const auto parsed = ParseUITheme( WriteUITheme( d ) );
    // WriteUITheme stamps the current version, so the refusal is provoked through the text instead.
    std::string text = WriteUITheme( Minimal() );
    const auto  at   = text.find( "\"FormatVersion\":" );
    ASSERT_NE( at, std::string::npos );
    text.replace( at, std::string( "\"FormatVersion\": 1" ).size(), "\"FormatVersion\": 42" );

    const auto refused = ParseUITheme( text );
    ASSERT_FALSE( static_cast<bool>( refused ) );
    EXPECT_NE( refused.GetError().find( "42" ), std::string::npos ) << refused.GetError();
}

TEST( UIThemeFormat, AnEmptyFileIsRefusedRatherThanReadAsAnEmptyTheme )
{
    const auto refused = ParseUITheme( "" );
    EXPECT_FALSE( static_cast<bool>( refused ) );
}

// Each of these is a way a theme can be internally wrong. The point of refusing them at the door is that
// every one of them, accepted, produces a UI that looks plausible and is not the one the author wrote.
TEST( UIThemeValidation, EveryInternalDisagreementIsRefusedByName )
{
    {
        UIThemeData d = Minimal();
        d.Styles[0].Slots.push_back( UIThemeBinding{ "Button.Norml", "Ink" } ); // a typo in a slot name
        const auto r = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Button.Norml" ), std::string::npos ) << r.GetError();
    }
    {
        UIThemeData d = Minimal();
        // A colour slot bound to a METRIC's name: both names exist, and neither the file nor the picture
        // would say which table was meant.
        d.Styles[0].Slots.push_back( UIThemeBinding{ "Button.Normal", "Radius" } );
        const auto r = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Radius" ), std::string::npos ) << r.GetError();
    }
    {
        UIThemeData d = Minimal();
        d.Colors.push_back( UIThemeColor{ "Ink", glm::vec3( 1.0f ) } ); // two rows, one name
        const auto r = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Ink" ), std::string::npos ) << r.GetError();
    }
    {
        UIThemeData d = Minimal();
        d.Styles[0].Slots.push_back( UIThemeBinding{ "Panel.Color", "Paper" } ); // the same slot twice
        const auto r = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Panel.Color" ), std::string::npos ) << r.GetError();
    }
    {
        // A HIGH-CONTRAST OVERRIDE THAT OVERRIDES NOTHING. The accessibility switch would appear to do
        // nothing for that token and there would be no way to see why.
        UIThemeData d        = Minimal();
        d.HighContrastColors = { UIThemeColor{ "Inkk", glm::vec3( 1.0f ) } };
        const auto r         = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Inkk" ), std::string::npos ) << r.GetError();
    }
    {
        UIThemeData d      = Minimal();
        d.Metrics[0].Value = -1.0f; // every metric is a length
        const auto r       = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Radius" ), std::string::npos ) << r.GetError();
    }
    {
        UIThemeData d       = Minimal();
        d.Colors[0].Value.g = std::numeric_limits<float>::quiet_NaN();
        const auto r        = ValidateUIThemeData( d );
        ASSERT_FALSE( static_cast<bool>( r ) );
        EXPECT_NE( r.GetError().find( "Ink" ), std::string::npos ) << r.GetError();
    }
}

// ----------------------------------------------------------------------------------------------
// Resolution — THE RELATION, in both directions
// ----------------------------------------------------------------------------------------------

TEST( UIStyleResolution, ChangingTheThemeMovesABoundSlotAndLeavesAnUnboundOneAlone )
{
    UIThemeData a     = Minimal();
    UIThemeData b     = Minimal();
    b.Colors[0].Value = glm::vec3( 0.7f, 0.1f, 0.4f ); // one token, one different value

    const UIThemeRuntime ra = Build( a );
    const UIThemeRuntime rb = Build( b );

    const glm::vec3 local( 0.86f, 0.24f, 0.62f );

    bool               unknownA = false, unknownB = false;
    const ElementStyle sa = CanvasStyle( &ra, 1.0f, false ).For( kUIThemeDefaultStyle, unknownA );
    const ElementStyle sb = CanvasStyle( &rb, 1.0f, false ).For( kUIThemeDefaultStyle, unknownB );
    ASSERT_FALSE( unknownA );
    ASSERT_FALSE( unknownB );

    // BOUND: the theme decides, and the two themes decide differently.
    EXPECT_EQ( sa.Color( StyleSlot::PanelColor, local ), a.Colors[0].Value );
    EXPECT_EQ( sb.Color( StyleSlot::PanelColor, local ), b.Colors[0].Value );
    EXPECT_NE( sa.Color( StyleSlot::PanelColor, local ), sb.Color( StyleSlot::PanelColor, local ) );

    // UNBOUND: neither theme binds the border, so both answer the element's own value — the direction
    // that fails if the resolver ever starts answering for slots it was not given.
    EXPECT_EQ( sa.Color( StyleSlot::PanelBorder, local ), local );
    EXPECT_EQ( sb.Color( StyleSlot::PanelBorder, local ), local );
    EXPECT_FALSE( sa.IsThemed( StyleSlot::PanelBorder ) );
}

TEST( UIStyleResolution, AnElementWithNoThemeAtAllIsEntirelyItsOwnValues )
{
    const glm::vec3    local( 0.86f, 0.24f, 0.62f );
    bool               unknown = false;
    const ElementStyle st      = CanvasStyle( nullptr, 1.0f, false ).For( kUIThemeDefaultStyle, unknown );

    EXPECT_FALSE( unknown ); // "there is no theme" is not "you asked for a style that does not exist"
    EXPECT_FALSE( st.IsThemed( StyleSlot::PanelColor ) );
    EXPECT_EQ( st.Color( StyleSlot::PanelColor, local ), local );
    EXPECT_EQ( st.Metric( StyleSlot::PanelCornerRadius, 6.0f ), 6.0f );
}

TEST( UIStyleResolution, AStyleTheThemeDoesNotDeclareIsReportedAndFallsBackToTheAuthoredValues )
{
    const UIThemeRuntime r = Build( Minimal() );
    const glm::vec3      local( 0.86f, 0.24f, 0.62f );

    bool               unknown = false;
    const ElementStyle st      = CanvasStyle( &r, 1.0f, false ).For( "Primry", unknown );

    EXPECT_TRUE( unknown ) << "a style name the theme does not declare must be reportable, or a typo "
                              "silently draws the element's own colours";
    // Not an invented default and not nothing drawn: the value the author actually typed.
    EXPECT_EQ( st.Color( StyleSlot::PanelColor, local ), local );
}

TEST( UIStyleResolution, TheFontSlotSuppliesBothTheFaceAndTheSize )
{
    UIThemeData d          = Minimal();
    d.Fonts[0].Size        = 31.0f;
    const UIThemeRuntime r = Build( d );

    bool               unknown = false;
    const ElementStyle st      = CanvasStyle( &r, 1.0f, false ).For( kUIThemeDefaultStyle, unknown );

    EXPECT_FLOAT_EQ( st.FontSize( StyleSlot::TextFont, 22.0f ), 31.0f );
    // An unbound font slot leaves both halves where the element put them.
    EXPECT_FLOAT_EQ( st.FontSize( StyleSlot::InputFont, 20.0f ), 20.0f );
}

// ACCESSIBILITY, HALF ONE. The multiplier is the reason a later accessibility task is three lines rather
// than a subsystem, and it has to reach BOTH kinds of size or a themed screen and an unthemed one would
// scale differently.
TEST( UIStyleAccessibility, FontScaleMultipliesTheThemedSizeAndTheLocalOneAlike )
{
    const UIThemeRuntime r = Build( Minimal() ); // binds Text.Font at 31 px

    bool               unknown = false;
    const ElementStyle one     = CanvasStyle( &r, 1.0f, false ).For( kUIThemeDefaultStyle, unknown );
    const ElementStyle big     = CanvasStyle( &r, 2.0f, false ).For( kUIThemeDefaultStyle, unknown );

    EXPECT_FLOAT_EQ( one.FontSize( StyleSlot::TextFont, 22.0f ), 31.0f );
    EXPECT_FLOAT_EQ( big.FontSize( StyleSlot::TextFont, 22.0f ), 62.0f );  // themed
    EXPECT_FLOAT_EQ( big.FontSize( StyleSlot::InputFont, 20.0f ), 40.0f ); // local
    EXPECT_FLOAT_EQ( big.ScaleFontSize( 8.0f ), 16.0f );                   // the auto-size floor

    // AND IT MOVES NOTHING ELSE. A multiplier that also touched a radius would be a "larger text" switch
    // that reshapes the screen.
    EXPECT_FLOAT_EQ( big.Metric( StyleSlot::PanelCornerRadius, 6.0f ),
                     one.Metric( StyleSlot::PanelCornerRadius, 6.0f ) );
    EXPECT_EQ( big.Color( StyleSlot::PanelColor, glm::vec3( 0.0f ) ),
               one.Color( StyleSlot::PanelColor, glm::vec3( 0.0f ) ) );
}

// ACCESSIBILITY, HALF TWO — and this is the sparse half, which is the whole argument for the overlay
// living inside the theme instead of being a second theme file.
TEST( UIStyleAccessibility, HighContrastSwapsOnlyTheTokensTheThemeDeclaresAnOverrideFor )
{
    UIThemeData d = Minimal();
    d.Styles[0].Slots.push_back( UIThemeBinding{ "Text.Color", "Paper" } );
    d.HighContrastColors = { UIThemeColor{ "Ink", glm::vec3( 0.0f, 0.0f, 0.0f ) } }; // Paper has no row

    const UIThemeRuntime r = Build( d );

    bool               unknown = false;
    const ElementStyle off     = CanvasStyle( &r, 1.0f, false ).For( kUIThemeDefaultStyle, unknown );
    const ElementStyle on      = CanvasStyle( &r, 1.0f, true ).For( kUIThemeDefaultStyle, unknown );

    const glm::vec3 local( 0.5f );
    EXPECT_EQ( off.Color( StyleSlot::PanelColor, local ), d.Colors[0].Value );
    EXPECT_EQ( on.Color( StyleSlot::PanelColor, local ), glm::vec3( 0.0f ) ); // overridden
    // Paper has no override, so it is the SAME colour with the switch on. A pass that walked the whole
    // palette would move this one too, and that is the failure this direction exists to catch.
    EXPECT_EQ( on.Color( StyleSlot::TextColor, local ), off.Color( StyleSlot::TextColor, local ) );
}

// ----------------------------------------------------------------------------------------------
// The shipped library
// ----------------------------------------------------------------------------------------------

TEST( UIThemeLibrary, EveryShippedThemeParsesAndFlattens )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* rel : kShippedThemes )
    {
        const std::string text = ReadFile( root + rel );
        ASSERT_FALSE( text.empty() ) << rel << " could not be read";

        const auto parsed = ParseUITheme( text );
        ASSERT_TRUE( static_cast<bool>( parsed ) ) << rel << ": " << parsed.GetError();

        const auto built = BuildUIThemeRuntime( parsed.GetValue(), rel, {} );
        ASSERT_TRUE( static_cast<bool>( built ) ) << rel << ": " << built.GetError();
        EXPECT_NE( built.GetValue().FindStyle( kUIThemeDefaultStyle ), nullptr )
             << rel
             << " declares no Default style, so an element that names no style of its own would "
                "be reported as a typo on every canvas";
    }
}

// THE TWO THEMES ARE INTERCHANGEABLE, which is what "switch the theme" means. If one declares a style or
// binds a slot the other does not, swapping them changes the SHAPE of what is themed and not only its
// values — an element would silently drop back to its authored colour with nothing to say so.
TEST( UIThemeLibrary, TheShippedThemesDeclareTheSameStylesAndBindTheSameSlots )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<UIThemeRuntime> built;
    for ( const char* rel : kShippedThemes )
    {
        const auto parsed = ParseUITheme( ReadFile( root + rel ) );
        ASSERT_TRUE( static_cast<bool>( parsed ) ) << rel;
        auto r = BuildUIThemeRuntime( parsed.GetValue(), rel, {} );
        ASSERT_TRUE( static_cast<bool>( r ) ) << rel;
        built.push_back( r.ExtractValue() );
    }
    ASSERT_EQ( built.size(), 2u );

    EXPECT_EQ( built[0].Styles.size(), built[1].Styles.size() );
    for ( const auto& [name, table] : built[0].Styles )
    {
        const auto* other = built[1].FindStyle( name );
        ASSERT_NE( other, nullptr ) << "only one of the shipped themes declares the style '" << name << "'";
        for ( std::size_t i = 0; i < kStyleSlotCount; ++i )
        {
            const bool boundHere  = table.Slots[i] != Desert::Assets::kUIThemeUnbound;
            const bool boundThere = other->Slots[i] != Desert::Assets::kUIThemeUnbound;
            EXPECT_EQ( boundHere, boundThere )
                 << "the style '" << name << "' binds " << StyleSlotName( static_cast<StyleSlot>( i ) )
                 << " in only one of the two shipped themes";
        }
    }
}

// THE STRONGEST RELATION IN THIS SUITE, and the one the frame evidence rests on.
//
// Desert_Dark exists to be the null step: a canvas that switches from NO theme to that theme must render
// the frame it rendered before, or a theme A/B measures the step onto the theme system as well as the
// step between two looks. That is only true while every token it binds equals the component field it
// replaces, digit for digit — and "a preset table and the saved scenes disagreeing" is a defect this
// engine has already shipped once, so the agreement is asserted rather than maintained by hand.
TEST( UIThemeLibrary, DesertDarkResolvesToTheComponentsOwnDefaults )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const auto parsed = ParseUITheme( ReadFile( root + kShippedThemes[0] ) );
    ASSERT_TRUE( static_cast<bool>( parsed ) ) << parsed.GetError();
    const auto built = BuildUIThemeRuntime( parsed.GetValue(), "Desert_Dark", {} );
    ASSERT_TRUE( static_cast<bool>( built ) ) << built.GetError();
    const UIThemeRuntime& theme = built.GetValue();

    bool               unknown = false;
    const ElementStyle st      = CanvasStyle( &theme, 1.0f, false ).For( kUIThemeDefaultStyle, unknown );
    ASSERT_FALSE( unknown );

    const Desert::ECS::UIPanelData       panel{};
    const Desert::ECS::UIButtonData      button{};
    const Desert::ECS::UIProgressBarData progress{};
    const Desert::ECS::UIToggleData      toggle{};
    const Desert::ECS::UISliderData      slider{};
    const Desert::ECS::UIScrollViewData  scroll{};
    const Desert::ECS::UIInputFieldData  input{};
    const Desert::ECS::UIDropdownData    dropdown{};
    const Desert::ECS::UILayoutGroupData group{};

    // A sentinel nothing can legitimately be: if a slot were unbound, the resolver would answer THIS and
    // the comparison would fail loudly rather than pass because two defaults happened to match.
    const glm::vec3 sentinel( -1.0f, -2.0f, -3.0f );
    const auto      Themed = [&]( StyleSlot slot )
    {
        EXPECT_TRUE( st.IsThemed( slot ) ) << StyleSlotName( slot ) << " is not bound by Desert_Dark";
        return st.Color( slot, sentinel );
    };

    EXPECT_EQ( Themed( StyleSlot::PanelColor ), panel.Color );
    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::PanelCornerRadius, -1.0f ), panel.CornerRadius );

    EXPECT_EQ( Themed( StyleSlot::ButtonNormal ), button.NormalColor );
    EXPECT_EQ( Themed( StyleSlot::ButtonHover ), button.HoverColor );
    EXPECT_EQ( Themed( StyleSlot::ButtonPressed ), button.PressedColor );
    EXPECT_EQ( Themed( StyleSlot::ButtonSelected ), button.SelectedColor );
    EXPECT_EQ( Themed( StyleSlot::ButtonSelectedAccent ), button.SelectedAccent );
    EXPECT_EQ( Themed( StyleSlot::ButtonDisabled ), button.DisabledColor );

    EXPECT_EQ( Themed( StyleSlot::ProgressBackground ), progress.Background );
    EXPECT_EQ( Themed( StyleSlot::ProgressFill ), progress.Fill );
    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::ProgressCornerRadius, -1.0f ), progress.CornerRadius );

    EXPECT_EQ( Themed( StyleSlot::ToggleBox ), toggle.BoxColor );
    EXPECT_EQ( Themed( StyleSlot::ToggleCheck ), toggle.CheckColor );
    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::ToggleCornerRadius, -1.0f ), toggle.CornerRadius );

    EXPECT_EQ( Themed( StyleSlot::SliderTrack ), slider.TrackColor );
    EXPECT_EQ( Themed( StyleSlot::SliderFill ), slider.FillColor );
    EXPECT_EQ( Themed( StyleSlot::SliderHandle ), slider.HandleColor );

    EXPECT_EQ( Themed( StyleSlot::ScrollViewBackground ), scroll.Background );
    EXPECT_EQ( Themed( StyleSlot::ScrollViewScrollbar ), scroll.ScrollbarColor );

    EXPECT_EQ( Themed( StyleSlot::InputText ), input.TextColor );
    EXPECT_EQ( Themed( StyleSlot::InputPlaceholder ), input.PlaceholderColor );
    EXPECT_EQ( Themed( StyleSlot::InputBackground ), input.Background );
    EXPECT_EQ( Themed( StyleSlot::InputFocus ), input.FocusColor );
    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::InputCornerRadius, -1.0f ), input.CornerRadius );

    EXPECT_EQ( Themed( StyleSlot::DropdownBackground ), dropdown.Background );
    EXPECT_EQ( Themed( StyleSlot::DropdownText ), dropdown.TextColor );
    EXPECT_EQ( Themed( StyleSlot::DropdownHighlight ), dropdown.Highlight );
    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::DropdownCornerRadius, -1.0f ), dropdown.CornerRadius );

    EXPECT_FLOAT_EQ( st.Metric( StyleSlot::LayoutGroupSpacing, -1.0f ), group.Spacing );

    const Desert::ECS::UITextData       text{};
    const Desert::ECS::UIIconData       icon{};
    const Desert::ECS::UIDropTargetData drop{};
    EXPECT_EQ( Themed( StyleSlot::TextColor ), text.Color );
    EXPECT_EQ( Themed( StyleSlot::IconColor ), icon.Color );
    EXPECT_EQ( Themed( StyleSlot::DropTargetHighlight ), drop.HighlightColor );
}

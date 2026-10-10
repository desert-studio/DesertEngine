// THE LAYOUT PANELS UE HAS AND THE GROUP DID NOT: WrapBox, Overlay, UniformGridPanel, SizeBox, ScaleBox,
// and Slate's flex-shrink in the VBox/HBox. Pure math in UI/UILayout.hpp (ArrangeLayoutGroup /
// MeasureLayoutGroup), so the invariants are asked of the solver directly, plus one round trip of the new
// component fields through JSON text — a panel that cannot be saved is a panel nobody can author.
//
// Invariants only (owner 10-08): no children = no size, a round trip brings every field back, and each
// panel's defining relation (wrap breaks where the next child would not fit, overlay children share one
// rect, uniform cells are equal, size box clamps, scale box scales the content as one picture).

#include <gtest/gtest.h>
#include "../../TestSupport/runner.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <UI/UILayout.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <string>
#include <vector>

namespace UI = Desert::UI;
using UI::LayoutGroupParams;
using UI::LayoutGroupType;
using UI::LayoutSlot;
using UI::Rect;

namespace
{
    LayoutGroupParams Params( LayoutGroupType type )
    {
        LayoutGroupParams p;
        p.Type = type;
        return p;
    }

    std::vector<LayoutSlot> Slots( const std::vector<glm::vec2>& prefs )
    {
        std::vector<LayoutSlot> out;
        for ( const glm::vec2& s : prefs )
        {
            LayoutSlot slot;
            slot.Pref = s;
            out.push_back( slot );
        }
        return out;
    }

    constexpr LayoutGroupType kAllTypes[] = { LayoutGroupType::Horizontal, LayoutGroupType::Vertical,
                                              LayoutGroupType::Grid,       LayoutGroupType::Wrap,
                                              LayoutGroupType::Overlay,    LayoutGroupType::UniformGrid,
                                              LayoutGroupType::SizeBox,    LayoutGroupType::ScaleBox };
} // namespace

// Zero children = zero size, for every panel, and zero arranged slots.
TEST( UILayoutPanels, NoChildrenMeasuresToNothingForEveryPanel )
{
    for ( const LayoutGroupType type : kAllTypes )
    {
        SCOPED_TRACE( static_cast<int>( type ) );
        const glm::vec2 size = UI::MeasureLayoutGroup( Params( type ), {} );
        EXPECT_FLOAT_EQ( size.x, 0.0f );
        EXPECT_FLOAT_EQ( size.y, 0.0f );
        EXPECT_TRUE( UI::ArrangeLayoutGroup( { 0, 0, 300, 200 }, Params( type ), {} ).empty() );
    }
}

TEST( UILayoutPanels, WrapBreaksWhereTheNextChildWouldNotFit )
{
    LayoutGroupParams p = Params( LayoutGroupType::Wrap );
    p.Spacing           = 10.0f;
    p.StretchCross      = false;
    // 100 + 10 + 100 = 210 fits in 250; the third (+110) would not, so it opens line two.
    const auto a =
         UI::ArrangeLayoutGroup( { 0, 0, 250, 500 }, p, Slots( { { 100, 40 }, { 100, 20 }, { 100, 30 } } ) );
    ASSERT_EQ( a.size(), 3u );
    EXPECT_FLOAT_EQ( a[0].R.X, 0.0f );
    EXPECT_FLOAT_EQ( a[1].R.X, 110.0f );
    EXPECT_FLOAT_EQ( a[2].R.X, 0.0f );
    EXPECT_FLOAT_EQ( a[2].R.Y, 50.0f ); // line one is 40 tall, plus the spacing

    p.WrapSize           = 250.0f;
    const glm::vec2 size = UI::MeasureLayoutGroup( p, { { 100, 40 }, { 100, 20 }, { 100, 30 } } );
    EXPECT_FLOAT_EQ( size.x, 210.0f );
    EXPECT_FLOAT_EQ( size.y, 80.0f );
}

TEST( UILayoutPanels, OverlayChildrenShareTheInnerRect )
{
    LayoutGroupParams p = Params( LayoutGroupType::Overlay );
    p.PaddingL = p.PaddingT = p.PaddingR = p.PaddingB = 5.0f;
    const auto a = UI::ArrangeLayoutGroup( { 10, 20, 200, 100 }, p, Slots( { { 50, 50 }, { 20, 80 } } ) );
    ASSERT_EQ( a.size(), 2u );
    for ( const auto& s : a )
    {
        EXPECT_FLOAT_EQ( s.R.X, 15.0f );
        EXPECT_FLOAT_EQ( s.R.Y, 25.0f );
        EXPECT_FLOAT_EQ( s.R.W, 190.0f );
        EXPECT_FLOAT_EQ( s.R.H, 90.0f );
    }
    const glm::vec2 size = UI::MeasureLayoutGroup( p, { { 50, 50 }, { 20, 80 } } );
    EXPECT_FLOAT_EQ( size.x, 60.0f );
    EXPECT_FLOAT_EQ( size.y, 90.0f );
}

TEST( UILayoutPanels, UniformGridCellsAreEqualAndSizedByTheLargestChild )
{
    LayoutGroupParams p                = Params( LayoutGroupType::UniformGrid );
    p.Columns                          = 2;
    const std::vector<glm::vec2> prefs = { { 10, 10 }, { 40, 20 }, { 30, 60 } };
    const glm::vec2              size  = UI::MeasureLayoutGroup( p, prefs );
    EXPECT_FLOAT_EQ( size.x, 80.0f );  // 2 columns of the widest (40)
    EXPECT_FLOAT_EQ( size.y, 120.0f ); // 2 rows of the tallest (60)

    const auto a = UI::ArrangeLayoutGroup( { 0, 0, 200, 100 }, p, Slots( prefs ) );
    ASSERT_EQ( a.size(), 3u );
    for ( const auto& s : a )
    {
        EXPECT_FLOAT_EQ( s.R.W, 100.0f );
        EXPECT_FLOAT_EQ( s.R.H, 50.0f );
    }
    EXPECT_FLOAT_EQ( a[2].R.X, 0.0f );
    EXPECT_FLOAT_EQ( a[2].R.Y, 50.0f );
}

TEST( UILayoutPanels, SizeBoxOverrideWinsAndMaxWinsOverMin )
{
    LayoutGroupParams p = Params( LayoutGroupType::SizeBox );
    p.SizeMin           = { 300.0f, 10.0f };
    p.SizeMax           = { 200.0f, -1.0f };
    glm::vec2 size      = UI::MeasureLayoutGroup( p, { { 50, 40 } } );
    EXPECT_FLOAT_EQ( size.x, 200.0f ); // min raised it to 300, max brought it back
    EXPECT_FLOAT_EQ( size.y, 40.0f );  // min 10 is below the content; no max

    p.SizeOverride = { -1.0f, 0.0f }; // a zero override is a legal size, not "unset"
    size           = UI::MeasureLayoutGroup( p, { { 50, 40 } } );
    EXPECT_FLOAT_EQ( size.x, 200.0f );
    EXPECT_FLOAT_EQ( size.y, 0.0f );
}

TEST( UILayoutPanels, ScaleBoxScalesTheContentAsOnePicture )
{
    LayoutGroupParams p = Params( LayoutGroupType::ScaleBox );
    const auto        a = UI::ArrangeLayoutGroup( { 0, 0, 400, 100 }, p, Slots( { { 100, 50 } } ) );
    ASSERT_EQ( a.size(), 1u );
    EXPECT_FLOAT_EQ( a[0].Scale, 2.0f ); // ScaleToFit: min(400/100, 100/50)
    EXPECT_FLOAT_EQ( a[0].R.W, 200.0f );
    EXPECT_FLOAT_EQ( a[0].R.X, 100.0f ); // centred

    p.StretchDirection = UI::LayoutScaleDirection::DownOnly;
    EXPECT_FLOAT_EQ( UI::ArrangeLayoutGroup( { 0, 0, 400, 100 }, p, Slots( { { 100, 50 } } ) )[0].Scale, 1.0f );

    p.StretchDirection = UI::LayoutScaleDirection::Both;
    p.Stretch          = UI::LayoutScaleStretch::ScaleToFill;
    EXPECT_FLOAT_EQ( UI::ArrangeLayoutGroup( { 0, 0, 400, 100 }, p, Slots( { { 100, 50 } } ) )[0].Scale, 4.0f );
}

// Slate StretchContent: overflow is taken back in proportion to Shrink * basis, never below Min, and a
// child with Shrink 0 keeps its size.
TEST( UILayoutPanels, FlexShrinkTakesTheOverflowBackByWeightAndStopsAtMin )
{
    LayoutGroupParams p = Params( LayoutGroupType::Horizontal );
    auto              s = Slots( { { 200, 10 }, { 100, 10 }, { 100, 10 } } );
    s[0].Shrink         = 1.0f;
    s[1].Shrink         = 1.0f;
    // 400 into 250: overflow 150 split 2:1 by Shrink*basis -> 100 and 50.
    auto a = UI::ArrangeLayoutGroup( { 0, 0, 250, 10 }, p, s );
    EXPECT_FLOAT_EQ( a[0].R.W, 100.0f );
    EXPECT_FLOAT_EQ( a[1].R.W, 50.0f );
    EXPECT_FLOAT_EQ( a[2].R.W, 100.0f );

    s[1].Min.x = 90.0f; // the second child stops at 90; the first takes the rest of the overflow
    a          = UI::ArrangeLayoutGroup( { 0, 0, 250, 10 }, p, s );
    EXPECT_FLOAT_EQ( a[1].R.W, 90.0f );
    EXPECT_NEAR( a[0].R.W, 60.0f, 1e-3f );
    EXPECT_NEAR( a[0].R.W + a[1].R.W + a[2].R.W, 250.0f, 1e-3f );

    // No shrink anywhere: the children keep their size and overflow, exactly as before shrink existed.
    a = UI::ArrangeLayoutGroup( { 0, 0, 250, 10 }, p, Slots( { { 200, 10 }, { 100, 10 } } ) );
    EXPECT_FLOAT_EQ( a[0].R.W, 200.0f );
    EXPECT_FLOAT_EQ( a[1].R.W, 100.0f );
}

namespace
{
    const Desert::Reflection::TypeInfo& Type( const std::string& name )
    {
        const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( name );
        EXPECT_NE( type, nullptr ) << name;
        return *type;
    }

    template <class T>
    T ThroughText( const T& written, const std::string& typeName )
    {
        const auto object = Desert::Reflection::SerializeReflected( Type( typeName ), &written, nullptr );
        const auto parsed = Common::Json::Read<Common::Json::Object>( Common::Json::Write( object ) );
        EXPECT_TRUE( parsed.IsSuccess() );
        const Common::Json::Value value( parsed.GetValue() ); // Root() refuses a temporary
        T                         read;
        Common::Json::Issues      issues;
        Desert::Reflection::DeserializeReflected( Type( typeName ), &read, Common::Json::Root( value ), issues,
                                                  nullptr );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
        return read;
    }
} // namespace

TEST( UILayoutPanels, EveryPanelFieldSurvivesSaveAndLoad )
{
    UI::UILayoutGroupData g;
    g.Type             = UI::UILayoutType::ScaleBox;
    g.WrapSize         = 321.0f;
    g.WrapVertical     = true;
    g.MinSlotSize      = { 12.0f, 34.0f };
    g.SizeMin          = { 1.0f, 2.0f };
    g.SizeMax          = { 300.0f, -1.0f };
    g.SizeOverride     = { -1.0f, 0.0f };
    g.Stretch          = UI::UIScaleStretch::UserSpecified;
    g.StretchDirection = UI::UIScaleDirection::UpOnly;
    g.UserScale        = 2.5f;
    const auto r       = ThroughText( g, "UILayoutGroupData" );
    EXPECT_EQ( r.Type, g.Type );
    EXPECT_FLOAT_EQ( r.WrapSize, g.WrapSize );
    EXPECT_EQ( r.WrapVertical, g.WrapVertical );
    EXPECT_EQ( r.MinSlotSize, g.MinSlotSize );
    EXPECT_EQ( r.SizeMin, g.SizeMin );
    EXPECT_EQ( r.SizeMax, g.SizeMax );
    EXPECT_EQ( r.SizeOverride, g.SizeOverride );
    EXPECT_EQ( r.Stretch, g.Stretch );
    EXPECT_EQ( r.StretchDirection, g.StretchDirection );
    EXPECT_FLOAT_EQ( r.UserScale, g.UserScale );

    UI::UILayoutData L;
    L.FlexShrink = 1.5f;
    EXPECT_FLOAT_EQ( ThroughText( L, "UILayoutData" ).FlexShrink, 1.5f );
}

namespace
{
    class StructLinksEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Desert::Reflection::ReflectionRegistry::Get().ResolveStructLinks();
        }
    };

    const Desert::TestSupport::SuiteEnvironment kStructLinks{
         +[]() -> ::testing::Environment*
         {
             return new StructLinksEnvironment; // NOLINT(cppcoreguidelines-owning-memory)
         } };
} // namespace

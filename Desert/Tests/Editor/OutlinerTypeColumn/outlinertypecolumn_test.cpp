// THE DEFECT THIS SUITE PINS.
//
// The outliner's Type column was a hard-coded 110 pixels and its text came from a chain of if-statements
// somewhere else in the file. The two never agreed: "StaticMeshActor" is wider than 110px at the editor's
// font, so the column whose entire job is to say WHAT a row is was clipped on essentially every entity in
// the Starter scene -- visible in the shipped editor, and invisible to every test, because each side was
// individually correct. A width is not wrong on its own. A name is not wrong on its own. Only the
// RELATION between them was, which is the defect shape this project has now paid for repeatedly.
//
// So the census is the single definition (Editor/Panels/SceneHierarchy/EntityTypeCensus.hpp) and this
// suite asserts the relation: the column the outliner asks for is wide enough for every name the outliner
// can print. The enum-vs-table half is a static_assert in the header, so a kind added without a row does
// not compile at all.

#include <gtest/gtest.h>

#include <Editor/Panels/SceneHierarchy/EntityTypeCensus.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

using namespace Desert::Editor;

namespace
{
    // A stand-in for ImGui::CalcTextSize().x. Proportional on purpose: a measure of "1 unit per character"
    // would let a bug that measures the WRONG string still produce the right number, because every string
    // of the same length would measure the same. Capitals and descenders cost more here, so the widest
    // name is genuinely the widest string and not merely the longest one.
    float FakeMeasure( std::string_view text )
    {
        float w = 0.0f;
        for ( const char c : text )
            w += ( c >= 'A' && c <= 'Z' ) ? 9.0f : ( c == ' ' ? 4.0f : 7.0f );
        return w;
    }
} // namespace

TEST( OutlinerTypeColumn, EveryNameFitsTheColumnItIsPrintedIn )
{
    constexpr float kPadding = 12.0f;
    const float     width    = TypeColumnWidth( FakeMeasure, kPadding );

    for ( const EntityTypeInfo& info : kEntityTypes )
    {
        // The whole point of the column, asserted for every row it can ever draw.
        EXPECT_LE( FakeMeasure( info.Name ), width ) << "clipped: " << info.Name;
        // And the padding really is spare room, not swallowed by the widest entry.
        EXPECT_LE( FakeMeasure( info.Name ) + kPadding, width + 0.001f ) << info.Name;
    }
}

TEST( OutlinerTypeColumn, WidthIsDrivenByTheWidestNameNotTheFirstOne )
{
    // A width computed from anything but the maximum (the first row, an average, a constant) passes the
    // test above for some names and fails for others. Pin the maximum itself.
    float widest = 0.0f;
    for ( const EntityTypeInfo& info : kEntityTypes )
        widest = std::max( widest, FakeMeasure( info.Name ) );

    EXPECT_FLOAT_EQ( TypeColumnWidth( FakeMeasure, 0.0f ), widest );
    EXPECT_FLOAT_EQ( TypeColumnWidth( FakeMeasure, 12.0f ), widest + 12.0f );
}

TEST( OutlinerTypeColumn, CensusIsCompleteAndUnambiguous )
{
    ASSERT_EQ( kEntityTypes.size(), static_cast<std::size_t>( EntityTypeKind::Count ) );

    std::set<std::string> seen;
    for ( std::size_t i = 0; i < kEntityTypes.size(); ++i )
    {
        const EntityTypeInfo& info = kEntityTypes[i];
        ASSERT_NE( info.Name, nullptr ) << "row " << i;
        EXPECT_FALSE( std::string_view( info.Name ).empty() ) << "row " << i;
        // Two kinds sharing a name would make the column say the same thing about two different things,
        // which is worse than clipping: it is confidently wrong.
        EXPECT_TRUE( seen.insert( info.Name ).second ) << "duplicate name: " << info.Name;

        // Colours are read straight into an ImVec4; a component outside [0,1] there does not fail, it
        // silently saturates, and the family the colour was supposed to signal is lost.
        for ( const float c : { info.R, info.G, info.B } )
        {
            EXPECT_GE( c, 0.0f ) << info.Name;
            EXPECT_LE( c, 1.0f ) << info.Name;
        }
    }
}

TEST( OutlinerTypeColumn, EntityTypeOfIndexesTheCensusInOrder )
{
    // EntityTypeOf is what the panel calls; the table is what the width is measured from. If the lookup
    // ever stopped being a plain index, the drawn name and the measured name would part company.
    for ( std::size_t i = 0; i < kEntityTypes.size(); ++i )
    {
        const EntityTypeKind kind = static_cast<EntityTypeKind>( i );
        EXPECT_STREQ( EntityTypeOf( kind ).Name, kEntityTypes[i].Name );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

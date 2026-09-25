// The Common::Json facade (JS1a): the one door between a struct and JSON. What is pinned here is the
// CONTRACT every caller now relies on instead of choosing rfl flags itself: a round trip is lossless, a
// missing field takes its default, an unknown field is ignored (or carried, when the struct asks), and every
// failure names the field path — and, for a file, the file.

#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace FacadeTest
{
    struct Inner
    {
        int              A = 7;
        std::vector<int> Values;
    };

    struct Doc
    {
        std::string        Name = "default";
        Inner              Nested;
        std::vector<Inner> List;
    };
    DESERT_JSON_STRUCT( Doc, "FacadeTestDoc", 3 )

    struct Carrier
    {
        int                       Known = 0;
        Common::Json::CarriedKeys Other;
    };

    struct WithPointer
    {
        int* Raw = nullptr;
    };

    class WithPrivate
    {
    public:
        int Get() const
        {
            return m_Value;
        }

    private:
        int m_Value = 0;
    };
} // namespace FacadeTest

using namespace FacadeTest;
namespace Json = Common::Json;

static_assert( Json::IsReflectable<Doc> );
static_assert( !Json::IsReflectable<WithPointer>, "a raw pointer has no JSON meaning" );
static_assert( !Json::IsReflectable<WithPrivate>, "reflection cannot see private members" );
static_assert( Json::HasFormat<Doc> && !Json::HasFormat<Inner> );
static_assert( Json::FormatOf<Doc>.Name == "FacadeTestDoc" && Json::FormatOf<Doc>.Version == 3 );

namespace
{
    std::filesystem::path TempDir()
    {
        const auto dir = std::filesystem::temp_directory_path() / "desert_json_facade_test";
        std::filesystem::create_directories( dir );
        return dir;
    }
} // namespace

TEST( JsonFacade, RoundTripIsLossless )
{
    Doc doc;
    doc.Name          = "scene";
    doc.Nested.A      = -3;
    doc.Nested.Values = { 1, 2, 3 };
    doc.List          = { Inner{ 4, { 5 } } };

    const std::string text = Json::Write( doc );
    const auto        back = Json::Read<Doc>( text );
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( Json::Write( back.GetValue() ), text );
    EXPECT_EQ( back.GetValue().Nested.Values, doc.Nested.Values );
    EXPECT_EQ( back.GetValue().List.at( 0 ).A, 4 );
}

TEST( JsonFacade, ErrorNamesTheFieldPath )
{
    const auto bad = Json::Read<Doc>( R"({"Name":"x","Nested":{"A":"seven","Values":[]}})" );
    ASSERT_FALSE( bad );
    EXPECT_NE( bad.GetError().find( "field 'Nested.A'" ), std::string::npos ) << bad.GetError();

    const auto deep = Json::Read<Doc>( R"({"List":[{"A":1,"Values":[1,"q"]}]})" );
    ASSERT_FALSE( deep );
    EXPECT_NE( deep.GetError().find( "field 'List.Values'" ), std::string::npos ) << deep.GetError();

    const auto broken = Json::Read<Doc>( R"({"Name":)" );
    ASSERT_FALSE( broken );
    EXPECT_NE( broken.GetError().find( "document:" ), std::string::npos ) << broken.GetError();
}

TEST( JsonFacade, MissingFieldTakesItsDefaultAndUnknownFieldIsIgnored )
{
    const auto doc = Json::Read<Doc>( R"({"Name":"only","FromANewerBuild":42})" );
    ASSERT_TRUE( doc ) << doc.GetError();
    EXPECT_EQ( doc.GetValue().Name, "only" );
    EXPECT_EQ( doc.GetValue().Nested.A, 7 );
    EXPECT_TRUE( doc.GetValue().List.empty() );
}

TEST( JsonFacade, CarriedKeysSurviveARoundTrip )
{
    const auto carrier = Json::Read<Carrier>( R"({"Known":1,"OtherBuild":{"x":2}})" );
    ASSERT_TRUE( carrier ) << carrier.GetError();
    const std::string text = Json::Write( carrier.GetValue() );
    EXPECT_NE( text.find( R"("OtherBuild":{"x":2})" ), std::string::npos ) << text;
    EXPECT_EQ( text.find( "Other\"" ), std::string::npos ) << "the carrier itself must not become a key: " << text;
}

TEST( JsonFacade, ObjectMembersKeepFileOrder )
{
    const auto members = Json::ObjectMembers( R"({"b":1,"a":[1, 2]})" );
    ASSERT_TRUE( members ) << members.GetError();
    ASSERT_EQ( members.GetValue().size(), 2u );
    EXPECT_EQ( members.GetValue()[0].first, "b" );
    EXPECT_EQ( members.GetValue()[1].second, "[1,2]" );
    EXPECT_FALSE( Json::ObjectMembers( "[1]" ) );
}

TEST( JsonFacade, FileFunctionsNameTheFile )
{
    const auto dir  = TempDir();
    const auto file = dir / "doc.json";

    Doc doc;
    doc.Name           = "on disk";
    const auto written = Json::WriteFileAtomic( file, doc );
    ASSERT_TRUE( written ) << written.GetError();

    const auto back = Json::ReadFile<Doc>( file );
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( back.GetValue().Name, "on disk" );

    // Written through the canonical layout: one member per line, not one line of JSON.
    const auto raw = Common::Utils::FileSystem::ReadFileContent( file );
    ASSERT_TRUE( raw );
    EXPECT_NE( raw.GetValue().find( '\n' ), raw.GetValue().size() - 1 ) << raw.GetValue();

    {
        std::ofstream corrupt( dir / "corrupt.json" );
        corrupt << R"({"Nested":{"A":true}})";
    }
    const auto bad = Json::ReadFile<Doc>( dir / "corrupt.json" );
    ASSERT_FALSE( bad );
    EXPECT_NE( bad.GetError().find( "corrupt.json" ), std::string::npos ) << bad.GetError();
    EXPECT_NE( bad.GetError().find( "field 'Nested.A'" ), std::string::npos ) << bad.GetError();

    const auto missing = Json::ReadFile<Doc>( dir / "absent.json" );
    ASSERT_FALSE( missing );
    EXPECT_NE( missing.GetError().find( "absent.json" ), std::string::npos ) << missing.GetError();

    std::filesystem::remove_all( dir );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

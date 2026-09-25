// The Common::Json facade (JS1a): the one door between a struct and JSON. What is pinned here is the
// CONTRACT every caller now relies on instead of choosing rfl flags itself: a round trip is lossless; reading
// is STRICT (a missing field and an unknown key are errors naming their path) unless the type itself is marked
// DESERT_JSON_LENIENT with a reason; a struct that asks carries unknown keys instead; every failure names the
// field path — and, for a file, the file.

#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
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

    // A settings-like type several builds share: the only kind allowed to default a missing field.
    struct Shared
    {
        int Old   = 1;
        int Added = 5;
    };
    DESERT_JSON_LENIENT( Shared, "written by several builds at once, each knowing a different field set" )

    struct Maybe
    {
        int                Required = 0;
        std::optional<int> Absent;
    };

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
static_assert( Json::IsLenient<Shared> && !Json::IsLenient<Doc> && !Json::IsLenient<Inner> );
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

TEST( JsonFacade, StrictTypeRefusesAMissingFieldWithItsPath )
{
    // Everything present but one nested member: the error names exactly that member, and nothing is defaulted.
    const auto doc = Json::Read<Doc>( R"({"Name":"only","Nested":{"Values":[]},"List":[]})" );
    ASSERT_FALSE( doc ) << "a missing field was filled from the in-struct default: "
                        << Json::Write( doc.GetValue() );
    EXPECT_NE( doc.GetError().find( "field 'Nested.A': missing" ), std::string::npos ) << doc.GetError();

    // Several at once: each one reported with its own path.
    const auto many = Json::Read<Doc>( R"({"Nested":{"A":1}})" );
    ASSERT_FALSE( many );
    for ( const char* path : { "field 'Name'", "field 'List'", "field 'Nested.Values'" } )
        EXPECT_NE( many.GetError().find( path ), std::string::npos ) << path << " in: " << many.GetError();
}

TEST( JsonFacade, StrictTypeRefusesAnUnknownKeyWithItsPath )
{
    const auto top =
         Json::Read<Doc>( R"({"Name":"a","Nested":{"A":1,"Values":[]},"List":[],"FromANewerBuild":42})" );
    ASSERT_FALSE( top ) << "an unknown key was silently ignored";
    EXPECT_NE( top.GetError().find( "field 'FromANewerBuild': unknown key" ), std::string::npos )
         << top.GetError();

    const auto nested = Json::Read<Doc>( R"({"Name":"a","Nested":{"A":1,"Values":[],"Typo":0},"List":[]})" );
    ASSERT_FALSE( nested );
    EXPECT_NE( nested.GetError().find( "field 'Nested.Typo': unknown key" ), std::string::npos )
         << nested.GetError();
}

TEST( JsonFacade, OnlyAnOptionalMemberMayBeAbsentFromAStrictType )
{
    const auto without = Json::Read<Maybe>( R"({"Required":3})" );
    ASSERT_TRUE( without ) << without.GetError();
    EXPECT_FALSE( without.GetValue().Absent.has_value() );

    const auto missingRequired = Json::Read<Maybe>( R"({"Absent":4})" );
    ASSERT_FALSE( missingRequired );
    EXPECT_NE( missingRequired.GetError().find( "field 'Required': missing" ), std::string::npos )
         << missingRequired.GetError();
}

TEST( JsonFacade, OnlyALenientTypeDefaultsAMissingFieldAndPassesAnUnknownKey )
{
    static_assert( Json::LenientReason<Shared>.size() >= 24 );
    const auto shared = Json::Read<Shared>( R"({"Old":9,"FromANewerBuild":42})" );
    ASSERT_TRUE( shared ) << shared.GetError();
    EXPECT_EQ( shared.GetValue().Old, 9 );
    EXPECT_EQ( shared.GetValue().Added, 5 );

    // The same text shape against a strict type: refused, so leniency is the mark's doing and nothing else's.
    EXPECT_FALSE( Json::Read<Doc>( R"({"Name":"x","FromANewerBuild":42})" ) );
}

TEST( JsonFacade, CarriedKeysSurviveARoundTrip )
{
    // Strict type, yet the unknown key is CAPTURED (the struct declared a carrier), not refused.
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

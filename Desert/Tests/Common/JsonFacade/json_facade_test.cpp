// The Common::Json facade (JS1a): the one door between a struct and JSON. What is pinned here is the
// CONTRACT every caller now relies on instead of choosing rfl flags itself: a round trip is lossless; a field
// missing from the text takes the struct's own default (UE's class-default rule, owner 2026-10-07); an unknown
// key is an error naming its path, unless the type is marked DESERT_JSON_PARTIAL with a reason (a probe of a
// few members of a larger document); a struct that asks carries unknown keys instead; every failure names the
// field path — and, for a file, the file.

#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <vector>

// In an anonymous namespace: every suite of the layer links into one runner, and two suites'
// namespace-scope types of one name would be an ODR violation the linker resolves silently.
namespace
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

    // A probe of one member of a larger document: the only kind allowed to pass an unknown key.
    struct Probe
    {
        int Wanted = 1;
    };
    DESERT_JSON_PARTIAL( Probe, "reads the one Wanted member of a whole test document; the rest is that document" )

    struct Maybe
    {
        int                Stated = 0;
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
        [[nodiscard]] int Get() const
        {
            return m_Value;
        }

    private:
        int m_Value = 0;
    };
} // namespace

namespace Json = Common::Json;

static_assert( Json::IsReflectable<Doc> );
static_assert( !Json::IsReflectable<WithPointer>, "a raw pointer has no JSON meaning" );
static_assert( !Json::IsReflectable<WithPrivate>, "reflection cannot see private members" );
static_assert( Json::HasFormat<Doc> && !Json::HasFormat<Inner> );
static_assert( Json::IsPartial<Probe> && !Json::IsPartial<Doc> && !Json::IsPartial<Inner> );
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

TEST( JsonFacade, AMissingFieldTakesTheStructDefault )
{
    // Everything present but one nested member: that member is the in-struct default, the rest is the text's.
    const auto doc = Json::Read<Doc>( R"({"Name":"only","Nested":{"Values":[2]},"List":[]})" );
    ASSERT_TRUE( doc ) << doc.GetError();
    EXPECT_EQ( doc.GetValue().Nested.A, 7 );
    EXPECT_EQ( doc.GetValue().Nested.Values, std::vector<int>{ 2 } );
    EXPECT_EQ( doc.GetValue().Name, "only" );

    // Several at once, top level and inside an array element: each is its own default.
    const auto many = Json::Read<Doc>( R"({"List":[{"Values":[1]}]})" );
    ASSERT_TRUE( many ) << many.GetError();
    EXPECT_EQ( many.GetValue().Name, "default" );
    EXPECT_EQ( many.GetValue().Nested.A, 7 );
    EXPECT_TRUE( many.GetValue().Nested.Values.empty() );
    ASSERT_EQ( many.GetValue().List.size(), 1u );
    EXPECT_EQ( many.GetValue().List[0].A, 7 );

    // The empty object is the default struct, written back identically.
    const auto empty = Json::Read<Doc>( "{}" );
    ASSERT_TRUE( empty ) << empty.GetError();
    EXPECT_EQ( Json::Write( empty.GetValue() ), Json::Write( Doc{} ) );
}

TEST( JsonFacade, AnUnknownKeyIsRefusedWithItsPath )
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

TEST( JsonFacade, AnAbsentOptionalStaysAbsent )
{
    // The default of an optional is "not stated": absence keeps meaning absence, it is never filled in.
    const auto without = Json::Read<Maybe>( R"({"Stated":3})" );
    ASSERT_TRUE( without ) << without.GetError();
    EXPECT_EQ( without.GetValue().Stated, 3 );
    EXPECT_FALSE( without.GetValue().Absent.has_value() );

    const auto with = Json::Read<Maybe>( R"({"Absent":4})" );
    ASSERT_TRUE( with ) << with.GetError();
    EXPECT_EQ( with.GetValue().Stated, 0 );
    EXPECT_EQ( with.GetValue().Absent, 4 );
}

TEST( JsonFacade, OnlyAPartialTypePassesAnUnknownKey )
{
    static_assert( Json::PartialReason<Probe>.size() >= 24 );
    const auto probe = Json::Read<Probe>( R"({"Wanted":9,"RestOfTheDocument":{"x":[1,2]}})" );
    ASSERT_TRUE( probe ) << probe.GetError();
    EXPECT_EQ( probe.GetValue().Wanted, 9 );

    // A partial type still defaults what it wants and the text does not state.
    const auto absent = Json::Read<Probe>( R"({"RestOfTheDocument":1})" );
    ASSERT_TRUE( absent ) << absent.GetError();
    EXPECT_EQ( absent.GetValue().Wanted, 1 );

    // The same text shape against a whole-document type: refused, so passing the key is the mark's doing.
    const auto whole = Json::Read<Doc>( R"({"Name":"x","RestOfTheDocument":42})" );
    ASSERT_FALSE( whole );
    EXPECT_NE( whole.GetError().find( "field 'RestOfTheDocument': unknown key" ), std::string::npos )
         << whole.GetError();
}

TEST( JsonFacade, CarriedKeysSurviveARoundTrip )
{
    // A whole-document type, yet the unknown key is CAPTURED (the struct declared a carrier), not refused.
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

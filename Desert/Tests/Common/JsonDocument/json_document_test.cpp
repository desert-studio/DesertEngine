// Untyped JSON documents through the facade (JS1c S1): Common/Json/Document.hpp. What is pinned here is the
// lead's wrong-type rule (a wrong-typed value is an Issue with its full path and the field keeps its value; a
// block with an issue is dropped whole), the path text an issue carries, the strict/open split for unknown
// keys, and that a value built from a struct writes the SAME bytes Json::Write writes for that struct (risk R2
// of the JS1c design) — so a save may build its tree without a text round trip.

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Document.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace DocumentTest
{
    enum class Mode
    {
        Off,
        Soft,
        Hard
    };

    struct Light
    {
        float        Intensity = 3.0f;
        bool         Enabled   = true;
        std::string  Name      = "key";
        int          Count     = 4;
        glm::vec3    Color     = { 1.0f, 0.5f, 0.25f };
        Mode         Shape     = Mode::Soft;
        Common::UUID Target    = Common::UUID( 99 );
    };

    struct Block
    {
        int         A = 1;
        std::string B = "b";
        float       C = 2.0f;
    };

    struct Strict
    {
        int                Required = 0;
        std::optional<int> Maybe;
    };

    struct Carrier
    {
        int                       Known = 0;
        Common::Json::CarriedKeys Other;
    };

    struct Shared
    {
        int Old   = 1;
        int Added = 5;
    };
    DESERT_JSON_LENIENT( Shared, "written by several builds at once, each knowing a different field set" )

    struct Nested
    {
        std::vector<float> Weights;
        Block              Inner;
    };

    // Every field kind a Ser struct of the engine carries, at the values where a value tree could spell a
    // number differently from the struct writer.
    struct Everything
    {
        float                      F0   = 0.1f;
        float                      F1   = 1e-7f;
        float                      F2   = 3.4028235e38f;
        float                      F3   = -0.0f;
        double                     D0   = 0.1;
        double                     D1   = 1.0 / 3.0;
        double                     D2   = 1e300;
        std::int64_t               I0   = std::numeric_limits<std::int64_t>::min();
        std::int64_t               I1   = std::numeric_limits<std::int64_t>::max();
        std::uint32_t              U0   = std::numeric_limits<std::uint32_t>::max();
        std::uint64_t              U1   = static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() );
        int                        N    = -42;
        bool                       Flag = false;
        std::string                Text = "quote\" slash\\ tab\t unicode \xC3\xA9 \xE2\x82\xAC";
        Mode                       Kind = Mode::Hard;
        std::optional<float>       Set  = 2.5f;
        std::optional<float>       Unset;
        std::vector<float>         List   = { 0.1f, 0.2f, 0.3f };
        std::vector<std::string>   Names  = { "a", "b" };
        std::map<std::string, int> Table  = { { "x", 1 }, { "y", 2 } };
        Nested                     Deep   = { { 0.7f }, { 5, "five", 0.3f } };
        std::vector<Block>         Blocks = { Block{}, Block{ 2, "c", 1e-3f } };
    };
} // namespace DocumentTest

using namespace DocumentTest;
namespace Json = Common::Json;

namespace
{
    Json::Value MustParse( std::string_view text )
    {
        auto parsed = Json::Parse( text );
        EXPECT_TRUE( parsed ) << parsed.GetError();
        return parsed ? parsed.ExtractValue() : Json::Value();
    }

    // Unwraps a result the test expects to hold (ResultStr refuses GetValue on a temporary).
    template <typename T>
    T Val( const Common::ResultStr<T>& result )
    {
        EXPECT_TRUE( result ) << result.GetError();
        return result ? result.GetValue() : T{};
    }

    // The member the test expects to be there (a missing one fails the test and reads as a null view).
    Json::Node Member( const Json::Node& node, std::string_view key )
    {
        const auto member = node.Find( key );
        EXPECT_TRUE( member ) << node.Where().ToString() << " has no member '" << key << "'";
        return member ? *member : Json::Node();
    }

    Json::Path LightPath()
    {
        return Json::Path{}.Key( "Entities" ).Record( "7" ).Key( "Light" );
    }
} // namespace

TEST( JsonDocument, PathTextNamesEntityComponentAndField )
{
    EXPECT_EQ( Json::Path{}.ToString(), "document" );
    EXPECT_EQ( Json::Path{}.Key( "Entities" ).Record( "4127" ).Key( "Foliage" ).Key( "Density" ).ToString(),
               "Entities[id=4127].Foliage.Density" );
    EXPECT_EQ( Json::Path{}.Key( "Points" ).Index( 2 ).Key( "X" ).ToString(), "Points[2].X" );
    // A path is a value: extending it does not change the original.
    const Json::Path base = Json::Path{}.Key( "A" );
    (void)base.Key( "B" );
    EXPECT_EQ( base.ToString(), "A" );
}

TEST( JsonDocument, ParseNamesTheByteOfASyntaxError )
{
    const auto bad = Json::Parse( R"({"a": 1,, "b": 2})" );
    ASSERT_FALSE( bad );
    EXPECT_NE( bad.GetError().find( "at byte 8" ), std::string::npos ) << bad.GetError();

    const auto good = MustParse( R"({"z":1,"a":[true,null,"s"],"m":{"k":1.5}})" );
    EXPECT_EQ( Json::Write( good ), R"({"z":1,"a":[true,null,"s"],"m":{"k":1.5}})" );
}

TEST( JsonDocument, KindsAndScalarGetters )
{
    const auto doc  = MustParse( R"({"b":true,"i":5,"r":5.0,"h":5.5,"big":9223372036854775808.0,"s":"x",)"
                                  R"("n":null,"a":[1],"o":{},"id":"18446744073709551615","bad":"12a","num":12})" );
    const auto root = Json::Root( doc );
    auto       at   = [&]( std::string_view key ) { return root.Find( key ).value(); };

    EXPECT_EQ( at( "b" ).GetKind(), Json::Kind::Bool );
    EXPECT_EQ( at( "i" ).GetKind(), Json::Kind::Integer );
    EXPECT_EQ( at( "r" ).GetKind(), Json::Kind::Real );
    EXPECT_EQ( at( "s" ).GetKind(), Json::Kind::String );
    EXPECT_EQ( at( "n" ).GetKind(), Json::Kind::Null );
    EXPECT_EQ( at( "a" ).GetKind(), Json::Kind::Array );
    EXPECT_EQ( at( "o" ).GetKind(), Json::Kind::Object );

    EXPECT_TRUE( Val( at( "b" ).AsBool() ) );
    EXPECT_FALSE( at( "i" ).AsBool() );
    EXPECT_EQ( Val( at( "i" ).AsNumber() ), 5.0 );
    EXPECT_EQ( Val( at( "h" ).AsNumber() ), 5.5 );
    EXPECT_FALSE( at( "s" ).AsNumber() );
    // The AsInteger rule: an integral real inside int64 is an integer; 5.5 and 2^63 are not.
    EXPECT_EQ( Val( at( "i" ).AsInteger() ), 5 );
    EXPECT_EQ( Val( at( "r" ).AsInteger() ), 5 );
    EXPECT_FALSE( at( "h" ).AsInteger() );
    EXPECT_FALSE( at( "big" ).AsInteger() );
    EXPECT_EQ( Val( at( "s" ).AsString() ), "x" );
    EXPECT_FALSE( at( "i" ).AsString() );
    // Ids are decimal strings, the full 64 bits; a number or a non-digit is not an id.
    EXPECT_EQ( static_cast<std::uint64_t>( Val( at( "id" ).AsUuid() ) ), ~std::uint64_t( 0 ) );
    EXPECT_FALSE( at( "bad" ).AsUuid() );
    EXPECT_FALSE( at( "num" ).AsUuid() );
    EXPECT_EQ( at( "num" ).AsString().GetError(), "num: expected string, found integer 12" );
}

TEST( JsonDocument, FindGetAndIterationKeepFileOrderAndPaths )
{
    const auto doc  = MustParse( R"({"z":1,"a":{"q":[10,20]},"m":3})" );
    const auto root = Json::Root( doc, Json::Path{}.Key( "Entities" ).Record( "7" ) );

    EXPECT_FALSE( root.Find( "absent" ) );
    EXPECT_FALSE( Member( root, "z" ).Find( "inside-a-number" ) );
    const auto missing = root.Get( "Light" );
    ASSERT_FALSE( missing );
    EXPECT_EQ( missing.GetError(), "Entities[id=7].Light: missing — the format requires it" );
    const auto notObject = Member( root, "z" ).Get( "k" );
    EXPECT_EQ( notObject.GetError(), "Entities[id=7].z: expected object, found integer 1" );

    std::vector<std::string> names;
    root.ForEachMember( [&]( std::string_view name, const Json::Node& member )
                        { names.push_back( std::string( name ) + "=" + member.Where().ToString() ); } );
    EXPECT_EQ( names,
               ( std::vector<std::string>{ "z=Entities[id=7].z", "a=Entities[id=7].a", "m=Entities[id=7].m" } ) );

    std::vector<std::string> elements;
    Val( Val( root.Get( "a" ) ).Get( "q" ) )
         .ForEachElement( [&]( std::size_t i, const Json::Node& element )
                          { elements.push_back( std::to_string( i ) + ":" + element.Where().ToString() ); } );
    EXPECT_EQ( elements, ( std::vector<std::string>{ "0:Entities[id=7].a.q[0]", "1:Entities[id=7].a.q[1]" } ) );
    EXPECT_EQ( &Member( root, "m" ).Raw(), &Member( root, "m" ).Raw() )
         << "a Node is a view: it must not copy the value it looks at";
}

// THE LEAD'S RULE: a wrong-typed value is an Issue with the full path, the field keeps its value, loading goes on.
TEST( JsonDocument, AWrongTypedValueIsAnIssueAndTheFieldKeepsItsValue )
{
    const auto doc  = MustParse( R"({"Intensity":"bright","Enabled":1,"Name":5,"Count":2.5,"Color":[1,2],)"
                                  R"("Shape":"Round","Target":42})" );
    const auto node = Json::Root( doc, LightPath() );

    Light        light;
    Json::Issues issues;
    node.ReadInto( "Intensity", light.Intensity, issues );
    node.ReadInto( "Enabled", light.Enabled, issues );
    node.ReadInto( "Name", light.Name, issues );
    node.ReadInto( "Count", light.Count, issues );
    node.ReadInto( "Color", light.Color, issues );
    node.ReadInto( "Shape", light.Shape, issues );
    node.ReadInto( "Target", light.Target, issues );

    const Light defaults;
    EXPECT_EQ( light.Intensity, defaults.Intensity ) << "a wrong-typed float must not become 0";
    EXPECT_EQ( light.Enabled, defaults.Enabled );
    EXPECT_EQ( light.Name, defaults.Name );
    EXPECT_EQ( light.Count, defaults.Count );
    EXPECT_EQ( light.Color, defaults.Color );
    EXPECT_EQ( light.Shape, defaults.Shape );
    EXPECT_EQ( light.Target, defaults.Target );

    ASSERT_EQ( issues.size(), 7u ) << "every wrong-typed field is reported, none is silently ignored";
    EXPECT_EQ( Json::Describe( issues[0] ),
               R"(Entities[id=7].Light.Intensity: expected number, found string "bright")" );
    EXPECT_EQ( Json::Describe( issues[1] ), "Entities[id=7].Light.Enabled: expected bool, found integer 1" );
    EXPECT_EQ( issues[2].Path, "Entities[id=7].Light.Name" );
    EXPECT_EQ( issues[3].Found, "real 2.5" );
    EXPECT_EQ( issues[4].Expected, "array of 3 numbers" );
    EXPECT_EQ( issues[5].Expected, "an enumerator name" );
    EXPECT_EQ( issues[6].Expected, "id (decimal string)" );

    const std::string report = Json::ReportIssues( issues, "Scenes/Test.desce" );
    EXPECT_NE( report.find( "Scenes/Test.desce: 7 values of the wrong type" ), std::string::npos ) << report;
    EXPECT_NE( report.find( "Entities[id=7].Light.Target: expected id" ), std::string::npos ) << report;
    EXPECT_EQ( Json::ReportIssues( {}, "clean" ), "" );
}

TEST( JsonDocument, RightTypedValuesAreReadAndAbsentKeysAreNotIssues )
{
    const auto doc  = MustParse( R"({"Intensity":7,"Enabled":false,"Name":"fill","Count":9.0,"Color":[0,1,0.5],)"
                                  R"("Shape":"Hard","Target":"12345678901234567890"})" );
    const auto node = Json::Root( doc, LightPath() );

    Light        light;
    Json::Issues issues;
    node.ReadInto( "Intensity", light.Intensity, issues );
    node.ReadInto( "Enabled", light.Enabled, issues );
    node.ReadInto( "Name", light.Name, issues );
    node.ReadInto( "Count", light.Count, issues );
    node.ReadInto( "Color", light.Color, issues );
    node.ReadInto( "Shape", light.Shape, issues );
    node.ReadInto( "Target", light.Target, issues );
    float absent = 11.0f;
    node.ReadInto( "NotInTheFile", absent, issues );

    EXPECT_TRUE( issues.empty() ) << Json::ReportIssues( issues, "test" );
    EXPECT_EQ( light.Intensity, 7.0f );
    EXPECT_FALSE( light.Enabled );
    EXPECT_EQ( light.Name, "fill" );
    EXPECT_EQ( light.Count, 9 );
    EXPECT_EQ( light.Color, glm::vec3( 0.0f, 1.0f, 0.5f ) );
    EXPECT_EQ( light.Shape, Mode::Hard );
    EXPECT_EQ( static_cast<std::uint64_t>( light.Target ), 12345678901234567890ull );
    EXPECT_EQ( absent, 11.0f ) << "absent = keep";

    // ReadInto on something that is not an object reports the object itself.
    Json::Issues onNumber;
    Member( Json::Root( doc, LightPath() ), "Count" ).ReadInto( "x", absent, onNumber );
    ASSERT_EQ( onNumber.size(), 1u );
    EXPECT_EQ( Json::Describe( onNumber[0] ), "Entities[id=7].Light.Count: expected object, found real 9.0" );
}

TEST( JsonDocument, AnIntegerOutsideTheFieldsRangeIsAnIssue )
{
    const auto    doc  = MustParse( R"({"u8":300,"i8":-129,"u32":-1,"ok":255,"frac":1.5})" );
    const auto    node = Json::Root( doc );
    std::uint8_t  u8   = 1;
    std::int8_t   i8   = 2;
    std::uint32_t u32  = 3;
    std::uint8_t  ok   = 4;
    int           frac = 5;
    Json::Issues  issues;
    node.ReadInto( "u8", u8, issues );
    node.ReadInto( "i8", i8, issues );
    node.ReadInto( "u32", u32, issues );
    node.ReadInto( "ok", ok, issues );
    node.ReadInto( "frac", frac, issues );
    EXPECT_EQ( u8, 1 );
    EXPECT_EQ( i8, 2 );
    EXPECT_EQ( u32, 3u );
    EXPECT_EQ( ok, 255 );
    EXPECT_EQ( frac, 5 );
    ASSERT_EQ( issues.size(), 4u );
    EXPECT_EQ( Json::Describe( issues[0] ), "u8: expected integer in [0, 255], found integer 300" );
    EXPECT_EQ( issues[3].Path, "frac" );
}

// A Ser-struct block: missing keeps the in-struct default, unknown is tolerated, a wrong type drops it WHOLE.
TEST( JsonDocument, ABlockIsReadWholeOrDroppedWhole )
{
    const auto doc  = MustParse( R"({"Good":{"A":5,"Future":true},"Bad":{"A":6,"B":7},"Arr":[1,2]})" );
    const auto root = Json::Root( doc, LightPath() );

    const auto good = Member( root, "Good" ).AsBlock<Block>();
    ASSERT_TRUE( good ) << good.GetError();
    EXPECT_EQ( good.GetValue().A, 5 );
    EXPECT_EQ( good.GetValue().B, "b" ) << "a missing member keeps its in-struct default";

    const auto bad = Member( root, "Bad" ).AsBlock<Block>();
    ASSERT_FALSE( bad );
    EXPECT_NE( bad.GetError().find( "Entities[id=7].Light.Bad: field 'B'" ), std::string::npos ) << bad.GetError();

    Block        target{ 100, "kept", 9.0f };
    Json::Issues issues;
    root.ReadInto( "Bad", target, issues );
    EXPECT_EQ( target.A, 100 ) << "the right-typed A of a broken block must not be applied: the block is atomic";
    EXPECT_EQ( target.B, "kept" );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, "Entities[id=7].Light.Bad" );
    EXPECT_EQ( issues[0].Expected, "a readable block (dropped whole)" );
    EXPECT_EQ( issues[0].Found.rfind( "field 'B'", 0 ), 0u ) << issues[0].Found;

    root.ReadInto( "Good", target, issues );
    EXPECT_EQ( target.A, 5 );
    EXPECT_EQ( target.B, "b" );
    EXPECT_EQ( issues.size(), 1u );

    std::vector<int> list;
    root.ReadInto( "Arr", list, issues );
    EXPECT_EQ( list, ( std::vector<int>{ 1, 2 } ) );
}

// As<T> is Json::Read's strictness on a subtree: unknown refused unless T carries it, missing refused unless
// lenient.
TEST( JsonDocument, AsIsStrictUnlessTheTypeSaysOpen )
{
    const auto doc =
         MustParse( R"({"Unknown":{"Required":1,"Extra":2},"Missing":{},"Carry":{"Known":3,"Extra":4},)"
                    R"("Shared":{"Old":2}})" );
    const auto root = Json::Root( doc, Json::Path{}.Key( "Settings" ) );

    const auto unknown = Member( root, "Unknown" ).As<Strict>();
    ASSERT_FALSE( unknown );
    EXPECT_EQ( unknown.GetError(),
               "Settings.Unknown: field 'Extra': unknown key — the format does not declare it" );

    const auto missing = Member( root, "Missing" ).As<Strict>();
    ASSERT_FALSE( missing );
    EXPECT_NE( missing.GetError().find( "field 'Required': missing" ), std::string::npos ) << missing.GetError();

    const auto carried = Member( root, "Carry" ).As<Carrier>();
    ASSERT_TRUE( carried ) << carried.GetError();
    EXPECT_EQ( carried.GetValue().Known, 3 );
    EXPECT_EQ( Json::Write( carried.GetValue() ), R"({"Known":3,"Extra":4})" )
         << "the unknown key is carried back";

    const auto shared = Member( root, "Shared" ).As<Shared>();
    ASSERT_TRUE( shared ) << shared.GetError();
    EXPECT_EQ( shared.GetValue().Added, 5 );
}

TEST( JsonDocument, ObjectBuilderKeepsInsertionOrderAndSpellsEachKind )
{
    const Json::Object inner = Json::ObjectBuilder().Set( "k", 1 ).Build();
    Json::Object       built = Json::ObjectBuilder()
                              .Set( "z", true )
                              .Set( "a", 2 )
                              .Set( "r", 0.5f )
                              .Set( "s", "text" )
                              .Set( "e", Mode::Soft )
                              .Set( "id", Common::UUID( 18446744073709551615ull ) )
                              .Set( "v", glm::vec2( 1.0f, 0.25f ) )
                              .Set( "o", inner )
                              .Set( "blk", Block{} )
                              .Set( "a", 3 )
                              .Build();
    EXPECT_EQ( Json::Write( Json::Value( built ) ),
               R"({"z":true,"a":3,"r":0.5,"s":"text","e":"Soft","id":"18446744073709551615","v":[1.0,0.25],)"
               R"("o":{"k":1},"blk":{"A":1,"B":"b","C":2.0}})" );
}

TEST( JsonDocument, SameComparesTheWrittenText )
{
    EXPECT_TRUE( Json::Same( MustParse( R"({"a":1,"b":[true]})" ), MustParse( R"({ "a": 1, "b": [ true ] })" ) ) );
    EXPECT_FALSE( Json::Same( MustParse( R"({"a":1})" ), MustParse( R"({"a":1.0})" ) ) );
    EXPECT_FALSE( Json::Same( MustParse( R"({"a":1,"b":2})" ), MustParse( R"({"b":2,"a":1})" ) ) );
}

TEST( JsonDocument, MemberNamesAreInDeclarationOrder )
{
    EXPECT_EQ( Json::MemberNames<Block>(), ( std::vector<std::string>{ "A", "B", "C" } ) );
    EXPECT_EQ( &Json::MemberNames<Block>(), &Json::MemberNames<Block>() ) << "computed once per type";
}

// Risk R2: a tree built from a struct must write the bytes Json::Write writes for it, raw and canonical, for
// every field kind — or the save could not drop its text round trip.
TEST( JsonDocument, FromStructWritesTheSameBytesAsTheStructWriter )
{
    const Everything value;
    const auto       tree = Json::FromStruct( value );
    EXPECT_EQ( Json::Write( tree ), Json::Write( value ) );

    const auto canonicalTree   = Json::WriteCanonical( tree );
    const auto canonicalStruct = Common::Content::CanonicalJsonTextOfWriterOutput( Json::Write( value ) );
    ASSERT_TRUE( canonicalTree ) << canonicalTree.GetError();
    ASSERT_TRUE( canonicalStruct ) << canonicalStruct.GetError();
    EXPECT_EQ( canonicalTree.GetValue(), canonicalStruct.GetValue() );

    // And the tree reads back into an equal struct.
    const auto back = Json::Root( tree ).As<Everything>();
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( Json::Write( back.GetValue() ), Json::Write( value ) );

    Carrier carrier;
    carrier.Known                           = 1;
    carrier.Other[std::string( "Foreign" )] = Json::Value( std::string( "kept" ) );
    EXPECT_EQ( Json::Write( Json::FromStruct( carrier ) ), Json::Write( carrier ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

#include <gtest/gtest.h>

#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <glm/glm.hpp>

#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

// Exercises the engine's REFLECT()-driven serializer (Desert::Reflection::Serialize/DeserializeReflected) —
// the single code path every reflected struct (SceneSettings, component data blocks, ...) round-trips
// through. Builds a TypeInfo by hand (as the DesertHeaderTool codegen would) so the test is fully headless
// and covers each FieldType, a nested struct, a byte-sized enum, and the missing-key "keep default" rule.

using namespace Desert::Reflection;

namespace
{
    // Reads `src` rooted at "Sample" and returns what the reader refused.
    Common::Json::Issues Read( const TypeInfo& type, void* obj, const Common::Json::Object& src,
                               const AssetResolver* resolver = nullptr )
    {
        const Common::Json::Value value( src );
        Common::Json::Issues      issues;
        DeserializeReflected( type, obj, Common::Json::Root( value, Common::Json::Path().Key( "Sample" ) ), issues,
                              resolver );
        return issues;
    }
} // namespace

namespace
{
    enum class Mode : uint8_t // enum class : uint8_t -> exercises the size-1 ReadIntBySize/WriteIntBySize path
    {
        A = 0,
        B = 1,
        C = 2,
    };

    struct Nested
    {
        float   X = 0.0f;
        int32_t Y = 0;
    };

    struct Sample
    {
        bool        Flag    = false;
        int32_t     Count   = 0;
        uint32_t    UCount  = 0;
        float       Scale   = 0.0f;
        double      Precise = 0.0;
        std::string Name;
        glm::vec2   V2{ 0.0f };
        glm::vec3   V3{ 0.0f };
        glm::vec4   V4{ 0.0f };
        Mode        ModeVal = Mode::A;
        Nested      Child;
    };

    FieldInfo Field( const char* name, FieldType type, std::size_t offset, std::size_t size )
    {
        FieldInfo f;
        f.Name   = name;
        f.Type   = type;
        f.Offset = offset;
        f.Size   = size;
        return f;
    }

    TypeInfo MakeNestedType()
    {
        TypeInfo t;
        t.Name = "Nested";
        t.Size = sizeof( Nested );
        t.Fields.push_back( Field( "X", FieldType::Float, offsetof( Nested, X ), sizeof( float ) ) );
        t.Fields.push_back( Field( "Y", FieldType::Int, offsetof( Nested, Y ), sizeof( int32_t ) ) );
        return t;
    }

    // Note: the nested TypeInfo must outlive every use (StructType is a raw pointer into it).
    const TypeInfo g_NestedType = MakeNestedType();

    TypeInfo MakeSampleType()
    {
        TypeInfo t;
        t.Name = "Sample";
        t.Size = sizeof( Sample );
        t.Fields.push_back( Field( "Flag", FieldType::Bool, offsetof( Sample, Flag ), sizeof( bool ) ) );
        t.Fields.push_back( Field( "Count", FieldType::Int, offsetof( Sample, Count ), sizeof( int32_t ) ) );
        t.Fields.push_back( Field( "UCount", FieldType::UInt, offsetof( Sample, UCount ), sizeof( uint32_t ) ) );
        t.Fields.push_back( Field( "Scale", FieldType::Float, offsetof( Sample, Scale ), sizeof( float ) ) );
        t.Fields.push_back( Field( "Precise", FieldType::Double, offsetof( Sample, Precise ), sizeof( double ) ) );
        t.Fields.push_back( Field( "Name", FieldType::String, offsetof( Sample, Name ), sizeof( std::string ) ) );
        t.Fields.push_back( Field( "V2", FieldType::Vec2, offsetof( Sample, V2 ), sizeof( glm::vec2 ) ) );
        t.Fields.push_back( Field( "V3", FieldType::Vec3, offsetof( Sample, V3 ), sizeof( glm::vec3 ) ) );
        t.Fields.push_back( Field( "V4", FieldType::Vec4, offsetof( Sample, V4 ), sizeof( glm::vec4 ) ) );
        t.Fields.push_back( Field( "ModeVal", FieldType::Enum, offsetof( Sample, ModeVal ), sizeof( Mode ) ) );

        FieldInfo child  = Field( "Child", FieldType::Struct, offsetof( Sample, Child ), sizeof( Nested ) );
        child.StructType = &g_NestedType;
        t.Fields.push_back( child );
        return t;
    }

    Sample MakePopulated()
    {
        Sample s;
        s.Flag    = true;
        s.Count   = -1234;
        s.UCount  = 4000000000u; // > INT32_MAX: verifies the UInt path isn't sign-truncated
        s.Scale   = 3.5f;
        s.Precise = 2.718281828;
        s.Name    = "hello reflection";
        s.V2      = { 1.0f, 2.0f };
        s.V3      = { 3.0f, 4.0f, 5.0f };
        s.V4      = { 6.0f, 7.0f, 8.0f, 9.0f };
        s.ModeVal = Mode::C;
        s.Child   = { 42.0f, 7 };
        return s;
    }
} // namespace

TEST( ReflectionSerializer, RoundTripsEveryFieldType )
{
    const TypeInfo type = MakeSampleType();
    const Sample   src  = MakePopulated();

    const Common::Json::Object obj = SerializeReflected( type, &src );

    Sample dst; // factory defaults
    EXPECT_TRUE( Read( type, &dst, obj ).empty() );

    EXPECT_EQ( dst.Flag, src.Flag );
    EXPECT_EQ( dst.Count, src.Count );
    EXPECT_EQ( dst.UCount, src.UCount );
    EXPECT_FLOAT_EQ( dst.Scale, src.Scale );
    EXPECT_DOUBLE_EQ( dst.Precise, src.Precise );
    EXPECT_EQ( dst.Name, src.Name );
    EXPECT_EQ( dst.V2, src.V2 );
    EXPECT_EQ( dst.V3, src.V3 );
    EXPECT_EQ( dst.V4, src.V4 );
    EXPECT_EQ( dst.ModeVal, src.ModeVal );
    EXPECT_FLOAT_EQ( dst.Child.X, src.Child.X );
    EXPECT_EQ( dst.Child.Y, src.Child.Y );
}

TEST( ReflectionSerializer, ByteSizedEnumDoesNotCorruptNeighbours )
{
    // ModeVal is a uint8_t enum sitting right before the Nested Child. A wrong-width write would smear
    // into Child; assert both survive when only the enum is the max value.
    const TypeInfo type = MakeSampleType();
    Sample         src  = MakePopulated();
    src.ModeVal         = Mode::C;
    src.Child           = { -1.5f, 99 };

    Sample dst;
    EXPECT_TRUE( Read( type, &dst, SerializeReflected( type, &src ) ).empty() );

    EXPECT_EQ( dst.ModeVal, Mode::C );
    EXPECT_FLOAT_EQ( dst.Child.X, -1.5f );
    EXPECT_EQ( dst.Child.Y, 99 );
}

TEST( ReflectionSerializer, MissingKeysKeepDefaults )
{
    // Forward/backward compatibility: a field absent from the serialized object must keep the destination's
    // current value (this is how old scene files load against a struct that gained new fields).
    const TypeInfo type = MakeSampleType();

    Sample dst;
    dst.Scale = 12.5f;
    dst.Name  = "unchanged";
    dst.Count = 777;

    Common::Json::Object partial; // no keys at all
    EXPECT_TRUE( Read( type, &dst, partial ).empty() );

    EXPECT_FLOAT_EQ( dst.Scale, 12.5f );
    EXPECT_EQ( dst.Name, "unchanged" );
    EXPECT_EQ( dst.Count, 777 );
}

// ---------------------------------------------------------------------------------------------------
// AN ASSET HANDLE IS 64 BITS AND A DOUBLE HOLDS 53 OF THEM.
//
// Every integral field used to be read back through `AsNumber`, which returns a `double`. For int32 and
// uint32 that is exact and nobody noticed; for an AssetHandle it is not, and an AssetHandle is where the
// engine keeps the identity of every texture, mesh, material and font. A handle is a 64-bit FNV-1a hash
// or an id read out of a cooked file, so it is above 2^53 (9 007 199 254 740 992) essentially always.
//
// Measured on a live one before anything was changed: 5355760296319878840 came back as
// 5355760296319879168 — 328 out, at a magnitude 594 times past the point where doubles stop counting
// integers. The corruption was OURS. A probe over reflect-cpp showed rfl::json writing and reading that
// same value exactly, and 16135626166276358966 (above 2^63, the id a shipped `.tex` actually carries)
// exactly as well; rfl::Generic keeps integers in an int64_t alternative and never touches a double.
//
// The values below are chosen ON the boundary and on both sides of it, so the assertion fails for the
// right reason rather than because one arbitrary number happened to round.
// ---------------------------------------------------------------------------------------------------

namespace
{
    struct Slot
    {
        uint64_t Handle = 0;
    };

    TypeInfo MakeSlotType()
    {
        TypeInfo t;
        t.Name = "Slot";
        t.Size = sizeof( Slot );
        t.Fields.push_back(
             Field( "Handle", FieldType::AssetHandle, offsetof( Slot, Handle ), sizeof( uint64_t ) ) );
        return t;
    }

    // The trip a handle actually takes: object -> Generic tree -> JSON TEXT -> Generic tree -> object.
    // Going through the text matters — the file on disk is the text, and a serializer that kept the value
    // in memory and lost it on the way to JSON would pass an in-memory-only round trip.
    uint64_t RoundTripThroughJson( uint64_t handle )
    {
        const TypeInfo type = MakeSlotType();
        const Slot     src{ handle };

        const std::string json = rfl::json::write( rfl::Generic( SerializeReflected( type, &src ) ) );

        const auto reread = rfl::json::read<rfl::Generic>( json );
        if ( !reread )
            return 0;
        const auto obj = reread.value().to_object();
        if ( !obj )
            return 0;

        Slot dst;
        EXPECT_TRUE( Read( type, &dst, obj.value() ).empty() );
        return dst.Handle;
    }
} // namespace

TEST( ReflectionSerializer, AnAssetHandleSurvivesTheJsonTripExactly )
{
    // THE measured value, first and on its own: this is the number a canvas background sprite was given
    // and the number that came back.
    EXPECT_EQ( RoundTripThroughJson( 5355760296319878840ull ), 5355760296319878840ull )
         << "the handle a scene stores is not the handle it loads. Every reference in the file then names "
            "an asset that does not exist, with nothing logged anywhere.";

    // And the boundary itself, from below and above, so a failure says WHERE the loss starts.
    constexpr uint64_t kExactLimit = 1ull << 53; // 9 007 199 254 740 992
    for ( const uint64_t handle : { kExactLimit - 1, kExactLimit, kExactLimit + 1, kExactLimit + 2,
                                    kExactLimit * 3 + 7, 16135626166276358966ull } )
    {
        EXPECT_EQ( RoundTripThroughJson( handle ), handle ) << "handle " << handle << " did not survive";
    }

    // 16135626166276358966 is above 2^63, which is the OTHER edge: rfl::Generic has only a signed 64-bit
    // alternative, so the value is stored as a negative integer and has to be reinterpreted rather than
    // converted on the way back. It is not a hypothetical — it is the id `T_Checker.tex` carries.
}

TEST( ReflectionSerializer, AnUnsetHandleIsStillZeroAfterTheTrip )
{
    // The companion the test above needs: 0 means "no asset" everywhere in the engine, so a serializer
    // that turned every handle into a constant would satisfy nothing here, and one that turned 0 into
    // something else would fill every empty slot in every scene.
    EXPECT_EQ( RoundTripThroughJson( 0 ), 0u );
}

TEST( ReflectionSerializer, AnAssetHandleStoredAsAPathIsRefusedWithoutAResolverRatherThanZeroed )
{
    // DC §1.4 at the field level. A file written by the scene serializer stores a PATH, and reading it
    // with no resolver cannot produce a handle — but it must not silently produce 0 either, because 0 is
    // a legitimate value ("unset") and the caller cannot tell the two apart.
    const TypeInfo type = MakeSlotType();

    Common::Json::Object obj;
    obj["Handle"] = std::string( "cooked:Textures/T_Checker.tex" );

    Slot dst{ 12345ull };
    EXPECT_TRUE( Read( type, &dst, obj ).empty() );

    EXPECT_EQ( dst.Handle, 12345ull ) << "a named asset that could not be resolved overwrote the field "
                                         "with 0, which reads as 'the artist left this empty'";
}

// ---------------------------------------------------------------------------------------------------
// THE WRONG-TYPE RULE (Common/Json/Document.hpp). A reflected Float holding a string used to become 0.0
// with nothing logged, and a wrong-typed Bool/String/Vec/Struct was skipped with nothing logged either.
// Now every one is an Issue naming its full path, the field keeps its value, and the walk continues.
// ---------------------------------------------------------------------------------------------------

TEST( ReflectionSerializer, AWrongTypedFloatIsAnIssueAndKeepsItsValue )
{
    const TypeInfo type = MakeSampleType();
    Sample         dst;
    dst.Scale = 12.5f;

    Common::Json::Object obj;
    obj["Scale"] = std::string( "large" );
    obj["Count"] = static_cast<int64_t>( 9 );
    const auto issues = Read( type, &dst, obj );

    EXPECT_FLOAT_EQ( dst.Scale, 12.5f ) << "a string in a float field was substituted";
    EXPECT_EQ( dst.Count, 9 ) << "the walk stopped at the bad field";
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, "Sample.Scale" );
    EXPECT_EQ( issues[0].Expected, "number" );
    EXPECT_NE( issues[0].Found.find( "large" ), std::string::npos ) << issues[0].Found;
}

TEST( ReflectionSerializer, EveryWrongTypedFieldKindIsAnIssueWithItsPath )
{
    const TypeInfo type = MakeSampleType();
    const Sample   before = MakePopulated();
    Sample         dst    = before;

    Common::Json::Object child;
    child["X"] = true;
    Common::Json::Object obj;
    obj["Flag"]    = std::string( "yes" );
    obj["Count"]   = static_cast<int64_t>( 1 ) << 40; // outside int32
    obj["UCount"]  = static_cast<int64_t>( -1 );
    obj["Precise"] = Common::Json::Object{};
    obj["Name"]    = 3.0;
    obj["V2"]      = Common::Json::Value::Array{ Common::Json::Value( 1.0 ) }; // wrong length
    obj["V3"]      = std::string( "1 2 3" );
    obj["V4"]      = Common::Json::Value::Array{ Common::Json::Value( 1.0 ), Common::Json::Value( 2.0 ),
                                                 Common::Json::Value( std::string( "3" ) ),
                                                 Common::Json::Value( 4.0 ) };
    obj["ModeVal"] = static_cast<int64_t>( 300 ); // does not fit the 1-byte enum
    obj["Child"]   = child;
    const auto issues = Read( type, &dst, obj );

    std::vector<std::string> paths;
    for ( const auto& issue : issues )
        paths.push_back( issue.Path );
    EXPECT_EQ( paths, ( std::vector<std::string>{ "Sample.Flag", "Sample.Count", "Sample.UCount", "Sample.Precise",
                                                   "Sample.Name", "Sample.V2", "Sample.V3", "Sample.V4",
                                                   "Sample.ModeVal", "Sample.Child.X" } ) );
    EXPECT_EQ( dst.Flag, before.Flag );
    EXPECT_EQ( dst.Count, before.Count );
    EXPECT_EQ( dst.UCount, before.UCount );
    EXPECT_DOUBLE_EQ( dst.Precise, before.Precise );
    EXPECT_EQ( dst.Name, before.Name );
    EXPECT_EQ( dst.V2, before.V2 );
    EXPECT_EQ( dst.V3, before.V3 );
    EXPECT_EQ( dst.V4, before.V4 ) << "a vec was read half-way";
    EXPECT_EQ( dst.ModeVal, before.ModeVal );
    EXPECT_FLOAT_EQ( dst.Child.X, before.Child.X );
}

TEST( ReflectionSerializer, AStructFieldThatIsNotAnObjectIsAnIssue )
{
    const TypeInfo type = MakeSampleType();
    Sample         dst  = MakePopulated();
    Common::Json::Object obj;
    obj["Child"]      = static_cast<int64_t>( 5 );
    const auto issues = Read( type, &dst, obj );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, "Sample.Child" );
    EXPECT_EQ( issues[0].Expected, "object" );
}

TEST( ReflectionSerializer, AHandleOfNoAcceptedFormIsAnIssue )
{
    const TypeInfo type = MakeSlotType();
    Slot           dst{ 777ull };
    Common::Json::Object obj;
    obj["Handle"]     = true;
    const auto issues = Read( type, &dst, obj );
    EXPECT_EQ( dst.Handle, 777ull );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, "Sample.Handle" );
}

TEST( ReflectionSerializer, AContainerReadsWholeOrKeepsItsValue )
{
    // The codegen's halves of a std::vector field, as DesertHeaderTool emits them.
    struct Holder
    {
        std::vector<float> Values{ 1.0f, 2.0f };
    };
    FieldInfo f            = Field( "Values", FieldType::Unknown, offsetof( Holder, Values ), sizeof( std::vector<float> ) );
    f.IsContainer          = true;
    f.SerializeContainer   = WriteContainer<std::vector<float>>;
    f.DeserializeContainer = ReadContainer<std::vector<float>>;
    TypeInfo type;
    type.Fields.push_back( f );

    Holder src;
    src.Values = { 3.0f, 4.0f, 5.0f };
    Holder dst;
    EXPECT_TRUE( Read( type, &dst, SerializeReflected( type, &src ) ).empty() );
    EXPECT_EQ( dst.Values, src.Values );

    Common::Json::Object bad;
    bad["Values"] = Common::Json::Value::Array{ Common::Json::Value( 1.0 ), Common::Json::Value( std::string( "x" ) ) };
    const auto issues = Read( type, &dst, bad );
    EXPECT_EQ( dst.Values, src.Values ) << "one bad element must not shorten the vector";
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, "Sample.Values[1]" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

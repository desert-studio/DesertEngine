// COMPONENT(...) ROWS: what the header tool reads from a marker and how a row finds its reflected type.
//
// The fixture is a component header as the engine writes one; each test reads its markers and bodies through
// the tool's own reader (Source/ComponentBlocks.hpp) and resolves them against a REFLECT() registry, as the
// tool does before rendering Engine/Generated/ReflectedComponentBlocks.gen.hpp.

#include <gtest/gtest.h>

#include <ComponentBlocks.hpp>

#include <string>
#include <vector>

using namespace Desert::HeaderTool;

namespace
{
    // The body of each struct below, as the tool slices it (from after '{' to the closing '}').
    const char* const kScriptBody = R"(
        COMPONENT( Key( "Script" ), Block( Data ), Run( ActorsAndUI ) )
        Desert::ECS::ScriptData Data;
        bool                    Dirty = false;
)";

    const char* const kCameraBody = R"(
        COMPONENT( Key( "Camera" ), Whole, Run( Actors ) )
        PROPERTY() float FieldOfView = 60.0f;
)";

    const std::vector<ReflectedTypeName> kRegistry = {
         { "Desert::ECS::ScriptData", "ScriptData" },
         { "Desert::ECS::CameraComponent", "CameraComponent" },
    };

    // The text between COMPONENT( and its closing parenthesis, as the tool hands it to ParseComponentMeta.
    std::string MarkerArgs( const std::string& body )
    {
        const size_t open  = body.find( "COMPONENT(" ) + std::string( "COMPONENT(" ).size();
        const size_t close = body.find( ")\n", open );
        return body.substr( open, close - open );
    }

    ComponentBlock Read( const std::string& body, const std::string& fqn, const std::string& where )
    {
        std::string    error;
        ComponentBlock c = ParseComponentMeta( MarkerArgs( body ), error );
        EXPECT_TRUE( error.empty() ) << error;
        c.fqn   = fqn;
        c.where = where;
        if ( !c.member.empty() )
            c.memberType = DeclaredMemberType( body, c.member ).value_or( "" );
        return c;
    }

    std::string ParseError( const std::string& args )
    {
        std::string error;
        ParseComponentMeta( args, error );
        return error;
    }
} // namespace

TEST( HeaderToolChecks, ABlockRowNamesItsMemberAndResolvesTheMembersType )
{
    std::vector<ComponentBlock> rows = { Read( kScriptBody, "Desert::ECS::ScriptComponent", "Script.hpp:3" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_TRUE( errors.empty() ) << errors.front();
    ASSERT_EQ( rows.size(), 1u );
    EXPECT_EQ( rows[0].key, "Script" );
    EXPECT_EQ( rows[0].member, "Data" );
    EXPECT_EQ( rows[0].memberType, "Desert::ECS::ScriptData" );
    EXPECT_EQ( rows[0].typeName, "ScriptData" );
    EXPECT_EQ( rows[0].run, "ActorsAndUI" );
}

TEST( HeaderToolChecks, AWholeRowIsTheReflectedComponentItself )
{
    std::vector<ComponentBlock> rows = { Read( kCameraBody, "Desert::ECS::CameraComponent", "Camera.hpp:3" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_TRUE( errors.empty() ) << errors.front();
    ASSERT_EQ( rows.size(), 1u );
    EXPECT_TRUE( rows[0].member.empty() );
    EXPECT_EQ( rows[0].typeName, "CameraComponent" );
    EXPECT_EQ( rows[0].run, "Actors" );
}

TEST( HeaderToolChecks, ComponentRowsAreOrderedByKeyNotByHeaderOrder )
{
    std::vector<ComponentBlock> rows = { Read( kScriptBody, "Desert::ECS::ScriptComponent", "Script.hpp:3" ),
                                         Read( kCameraBody, "Desert::ECS::CameraComponent", "Camera.hpp:3" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_TRUE( errors.empty() ) << errors.front();
    ASSERT_EQ( rows.size(), 2u );
    EXPECT_EQ( rows[0].key, "Camera" );
    EXPECT_EQ( rows[1].key, "Script" );
}

TEST( HeaderToolChecks, ABlockOfAnUnreflectedTypeIsAnErrorAtTheMarker )
{
    const std::string           body = R"(
        COMPONENT( Key( "Tag" ), Block( Data ), Run( Actors ) )
        Desert::ECS::TagData Data;
)";
    std::vector<ComponentBlock> rows = { Read( body, "Desert::ECS::TagComponent", "Tag.hpp:7" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_EQ( errors.size(), 1u );
    EXPECT_EQ( errors[0].rfind( "Tag.hpp:7: COMPONENT Tag", 0 ), 0u ) << errors[0];
    EXPECT_NE( errors[0].find( "is not a REFLECT() type" ), std::string::npos ) << errors[0];
}

TEST( HeaderToolChecks, AWholeComponentThatIsNotReflectedIsAnError )
{
    std::vector<ComponentBlock> rows = { Read( kCameraBody, "Desert::ECS::LensComponent", "Lens.hpp:2" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_EQ( errors.size(), 1u );
    EXPECT_NE( errors[0].find( "is Whole but Desert::ECS::LensComponent" ), std::string::npos ) << errors[0];
}

TEST( HeaderToolChecks, AKeyStatedTwiceIsAnErrorNamingBothMarkers )
{
    std::vector<ComponentBlock> rows = { Read( kScriptBody, "Desert::ECS::ScriptComponent", "Script.hpp:3" ),
                                         Read( kScriptBody, "Desert::ECS::OtherScript", "Other.hpp:9" ) };
    std::vector<std::string>    errors;
    ResolveComponents( kRegistry, rows, errors );
    ASSERT_EQ( errors.size(), 1u );
    EXPECT_NE( errors[0].find( "COMPONENT key 'Script'" ), std::string::npos ) << errors[0];
    EXPECT_NE( errors[0].find( "Script.hpp:3" ), std::string::npos ) << errors[0];
    EXPECT_NE( errors[0].find( "Other.hpp:9" ), std::string::npos ) << errors[0];
}

TEST( HeaderToolChecks, AMarkerMissingOrMisspellingAnAttributeIsAnError )
{
    EXPECT_NE( ParseError( R"(Key( "A" ), Block( Data ), Runn( Actors ))" ).find( "unknown attribute" ),
               std::string::npos );
    EXPECT_NE( ParseError( R"(Block( Data ), Run( Actors ))" ).find( "Key(\"...\") is required" ),
               std::string::npos );
    EXPECT_NE( ParseError( R"(Key( "A" ), Block( Data ))" ).find( "Run( ... ) is required" ), std::string::npos );
    EXPECT_NE( ParseError( R"(Key( "A" ), Block( Data ), Whole, Run( Actors ))" ).find( "exactly one of" ),
               std::string::npos );
    EXPECT_NE( ParseError( R"(Key( "A" ), Run( Actors ))" ).find( "exactly one of" ), std::string::npos );
}

TEST( HeaderToolChecks, AMemberTypeIsReadOnlyFromADeclarationOfThatMember )
{
    EXPECT_EQ( DeclaredMemberType( kScriptBody, "Data" ).value_or( "" ), "Desert::ECS::ScriptData" );
    EXPECT_EQ( DeclaredMemberType( "float Database = 1.0f;", "Data" ), std::nullopt );
}

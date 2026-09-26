// SCHEMA v22: A MESH BUILT IN THE EDITOR IS SAVED AS ITS EditMesh (M4), and a v21 file's render arrays become one.
//
// v21 stored `StaticMesh.CustomVertices` (Position, Normal, TexCoord per RENDER vertex) + `CustomIndices`. The
// step welds them with Geometry::FromRenderMesh - the editor's own lift - into an EditMesh and stores its saved
// form under `StaticMesh.EditMesh`. The tracked corpus holds no such block (measured when the step was
// written), so the inputs here are synthetic, spelled as the v21 writer spelled them.
//
// WHAT IS ASSERTED IS THE RELATION a person cares about: the migrated scene, loaded the way the engine loads
// it (ReadBlock + FromSerialized + ToRenderMesh), draws the SAME triangles with the same positions, normals and
// UVs the v21 file described - and the topology is now real (a cube's 24 render vertices are 8 corners).

#include <SceneMigration.hpp>
#include <assets_sandbox.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using Desert::Migration::EditMeshMigrationReport;
using Desert::Migration::MigrateEditMeshV21ToV22;

namespace
{
    struct V
    {
        float px, py, pz, nx, ny, nz, u, v;
    };

    // A StaticMesh payload exactly as ComponentRegistry wrote it at v21, with a neighbour field so the step
    // has to carry the rest of the block across.
    std::string V21Payload( const std::vector<V>& vertices, const std::vector<unsigned>& indices )
    {
        std::string json =
             R"({"MaterialPaths":["Materials/Starter_Prop.demat"],"CastShadows":false,"CustomVertices":[)";
        for ( size_t i = 0; i < vertices.size(); ++i )
        {
            const V& v = vertices[i];
            json += ( ( i != 0u ) ? "," : "" ) + std::string( "{\"Position\":[" ) + std::to_string( v.px ) + "," +
                    std::to_string( v.py ) + "," + std::to_string( v.pz ) + "],\"Normal\":[" +
                    std::to_string( v.nx ) + "," + std::to_string( v.ny ) + "," + std::to_string( v.nz ) +
                    "],\"TexCoord\":[" + std::to_string( v.u ) + "," + std::to_string( v.v ) + "]}";
        }
        json += "],\"CustomIndices\":[";
        for ( size_t i = 0; i < indices.size(); ++i )
            json += ( ( i != 0u ) ? "," : "" ) + std::to_string( indices[i] );
        return json + "]}";
    }

    Desert::Assets::EntityData EntityWith( const std::string& tag, const std::string& payloadJson )
    {
        Desert::Assets::EntityData entity;
        entity.Tag  = tag;
        auto parsed = rfl::json::read<rfl::Generic>( payloadJson );
        EXPECT_TRUE( parsed.has_value() ) << payloadJson;
        if ( parsed.has_value() )
            entity.Components["StaticMesh"] = parsed.value();
        return entity;
    }

    // An axis-aligned cube, 100 cm, the way a render buffer holds one: 4 vertices per face, hard normals,
    // per-face UVs - 24 render vertices over 8 corners.
    void MakeCube( std::vector<V>& vertices, std::vector<unsigned>& indices )
    {
        const float s = 50.0f;
        struct Face
        {
            float n[3], u[3], v[3];
        };
        const Face faces[6] = {
             { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
             { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
             { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
        for ( const Face& f : faces )
        {
            const auto  base  = static_cast<unsigned>( vertices.size() );
            const float cu[4] = { -1, 1, 1, -1 };
            const float cv[4] = { -1, -1, 1, 1 };
            for ( int k = 0; k < 4; ++k )
            {
                V v{};
                v.px = s * ( f.n[0] + cu[k] * f.u[0] + cv[k] * f.v[0] );
                v.py = s * ( f.n[1] + cu[k] * f.u[1] + cv[k] * f.v[1] );
                v.pz = s * ( f.n[2] + cu[k] * f.u[2] + cv[k] * f.v[2] );
                v.nx = f.n[0], v.ny = f.n[1], v.nz = f.n[2];
                v.u = ( cu[k] + 1 ) * 0.5f, v.v = ( cv[k] + 1 ) * 0.5f;
                vertices.push_back( v );
            }
            for ( unsigned const i : { 0u, 1u, 2u, 2u, 3u, 0u } )
                indices.push_back( base + i );
        }
    }

    std::optional<Desert::Geometry::EditMesh> LoadedEditMesh( const Desert::Assets::EntityData& entity )
    {
        const auto payload = entity.Components.get( "StaticMesh" );
        if ( !payload.has_value() )
            return std::nullopt;
        const auto block = Desert::Core::Serialize::ReadBlock<Desert::Assets::StaticMeshComponentSer>(
             payload.value(), "StaticMesh" );
        if ( !block || !block->EditMesh )
            return std::nullopt;
        auto mesh = Desert::Geometry::FromSerialized( *block->EditMesh );
        EXPECT_TRUE( mesh.IsSuccess() ) << ( mesh.IsSuccess() ? "" : mesh.GetError() );
        if ( !mesh.IsSuccess() )
            return std::nullopt;
        return mesh.ExtractValue();
    }

    bool Has( const Desert::Assets::EntityData& entity, const char* key )
    {
        const auto payload = entity.Components.get( "StaticMesh" );
        return payload.has_value() && payload.value().to_object().has_value() &&
               payload.value().to_object().value().get( key ).has_value();
    }
} // namespace

TEST( SceneEditMeshMigration, ACubesRenderArraysBecomeEightCornersAndDrawTheSameTriangles )
{
    std::vector<V>        vertices;
    std::vector<unsigned> indices;
    MakeCube( vertices, indices );

    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWith( "Blockout", V21Payload( vertices, indices ) ) );

    const EditMeshMigrationReport report = MigrateEditMeshV21ToV22( entities );
    EXPECT_EQ( report.Entities, 1 );
    EXPECT_EQ( report.Rejected, 0 );
    ASSERT_EQ( report.ConvertedNames.size(), 1u );
    EXPECT_NE( report.ConvertedNames[0].find( "24 render vertices -> 8 vertices / 12 triangles" ),
               std::string::npos )
         << report.ConvertedNames[0];

    EXPECT_FALSE( Has( entities[0], "CustomVertices" ) );
    EXPECT_FALSE( Has( entities[0], "CustomIndices" ) );
    EXPECT_TRUE( Has( entities[0], "CastShadows" ) ) << "the rest of the block was not carried across";
    EXPECT_TRUE( Has( entities[0], "MaterialPaths" ) );

    const auto mesh = LoadedEditMesh( entities[0] );
    ASSERT_TRUE( mesh.has_value() );
    EXPECT_EQ( mesh.value().VertexCount(), 8 );    // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_EQ( mesh.value().TriangleCount(), 12 ); // NOLINT(bugprone-unchecked-optional-access)
    EXPECT_TRUE( mesh.value().IsManifold() );      // NOLINT(bugprone-unchecked-optional-access)
    // v21 never stored a tangent: the honest statement of that is no layer, not a layer of zeros.
    EXPECT_EQ( mesh.value().Attributes().Tangents(), nullptr ); // NOLINT(bugprone-unchecked-optional-access)

    // Drawn, it is the v21 cube: every v21 triangle appears with the same three (position, normal, UV)
    // corners. Compared as a multiset of corner triples, because the render order is the conversion's own.
    auto render = Desert::Geometry::ToRenderMesh( mesh.value() ); // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    const auto& out = render.GetValue();
    EXPECT_EQ( out.Vertices.size(), 24u );
    ASSERT_EQ( out.Submeshes.size(), 1u );
    using Corner   = std::tuple<float, float, float, float, float, float, float, float>;
    using Triangle = std::vector<Corner>;
    std::map<Triangle, int> expected;
    std::map<Triangle, int> actual;
    const auto              sorted = []( Triangle t )
    {
        std::sort( t.begin(), t.end() );
        return t;
    };
    for ( size_t i = 0; i < indices.size(); i += 3 )
    {
        Triangle t;
        for ( int j = 0; j < 3; ++j )
        {
            const V& v = vertices[indices[i + j]];
            t.emplace_back( v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.u, v.v );
        }
        expected[sorted( t )] += 1;
    }
    for ( const auto& tri : out.Indices )
    {
        Triangle t;
        for ( const uint32_t k : { tri.V1, tri.V2, tri.V3 } )
        {
            const auto& v = out.Vertices[k];
            t.emplace_back( v.Position.x, v.Position.y, v.Position.z, v.Normal.x, v.Normal.y, v.Normal.z,
                            v.TexCoord.x, v.TexCoord.y );
        }
        actual[sorted( t )] += 1;
    }
    EXPECT_EQ( actual, expected );
}

TEST( SceneEditMeshMigration, ABlockWithoutRenderArraysIsLeftByteIdentical )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back(
         EntityWith( "Prop", R"({"MaterialPaths":["Materials/Starter_Prop.demat"],"Primitive":"Cube"})" ) );
    const std::string before = rfl::json::write( entities[0].Components );

    const EditMeshMigrationReport report = MigrateEditMeshV21ToV22( entities );
    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Rejected, 0 );
    EXPECT_EQ( rfl::json::write( entities[0].Components ), before );
}

TEST( SceneEditMeshMigration, RunningTheStepTwiceChangesNothingTheSecondTime )
{
    std::vector<V>        vertices;
    std::vector<unsigned> indices;
    MakeCube( vertices, indices );
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWith( "Blockout", V21Payload( vertices, indices ) ) );

    (void)MigrateEditMeshV21ToV22( entities );
    const std::string once  = rfl::json::write( entities[0].Components );
    const auto        again = MigrateEditMeshV21ToV22( entities );
    EXPECT_EQ( again.Entities, 0 );
    EXPECT_EQ( rfl::json::write( entities[0].Components ), once );
}

TEST( SceneEditMeshMigration, ABlockItCannotReadIsNamedAndLeftInPlace )
{
    std::vector<Desert::Assets::EntityData> entities;
    entities.push_back( EntityWith( "HalfOnly", R"({"CustomVertices":[]})" ) );
    entities.push_back( EntityWith(
         "BadIndex",
         V21Payload( { { 0, 0, 0, 0, 1, 0, 0, 0 }, { 1, 0, 0, 0, 1, 0, 1, 0 }, { 0, 0, 1, 0, 1, 0, 0, 1 } },
                     { 0, 1, 7 } ) ) );
    entities.push_back( EntityWith( "Ragged", V21Payload( {}, { 0, 1 } ) ) );
    const std::string before = rfl::json::write( entities );

    const EditMeshMigrationReport report = MigrateEditMeshV21ToV22( entities );
    EXPECT_EQ( report.Entities, 0 );
    EXPECT_EQ( report.Rejected, 3 );
    ASSERT_EQ( report.RejectedNames.size(), 3u );
    EXPECT_NE( report.RejectedNames[0].find( "HalfOnly" ), std::string::npos );
    EXPECT_NE( report.RejectedNames[0].find( "without the other" ), std::string::npos ) << report.RejectedNames[0];
    EXPECT_NE( report.RejectedNames[1].find( "BadIndex" ), std::string::npos ) << report.RejectedNames[1];
    EXPECT_NE( report.RejectedNames[2].find( "whole number of triangles" ), std::string::npos )
         << report.RejectedNames[2];
    EXPECT_EQ( rfl::json::write( entities ), before );
}

TEST( SceneEditMeshMigration, AV21SceneIsRaisedToV22ThroughTheWholeChain )
{
    const Desert::TestSupport::AssetsSandbox sandbox( "SceneEditMeshMigration",
                                                      { "Materials/Starter_Prop.demat" } );
    std::vector<V>        vertices;
    std::vector<unsigned> indices;
    MakeCube( vertices, indices );

    Desert::Migration::SceneSerialized scene;
    scene.SceneName    = "Synthetic v21";
    scene.SceneVersion = 21;
    scene.UnitVersion  = Desert::Core::kUnitVersion;
    scene.Entities.push_back( EntityWith( "Blockout", V21Payload( vertices, indices ) ) );

    const auto report = Desert::Migration::MigrateScene( scene );
    EXPECT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.EditMeshRaised );
    EXPECT_FALSE( report.AnimGraphRaised ) << "a v21 file must not go back through the v21 step";
    EXPECT_EQ( report.EditMesh.Entities, 1 );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );
    EXPECT_TRUE( LoadedEditMesh( scene.Entities[0] ).has_value() );
}

TEST( SceneEditMeshMigration, TheStepFollowsTheAnimGraphStep )
{
    // The head assertion travelled to SceneProceduralTerrainMigration with v23; what stays is the order.
    EXPECT_EQ( Desert::Migration::kSceneVersionEditMesh, Desert::Migration::kSceneVersionAnimGraphAsset + 1 )
         << "two steps share a number, or one was skipped - see kSceneVersionTextKeySigil's note";
    EXPECT_LT( Desert::Migration::kSceneVersionEditMesh, Desert::Core::kSceneVersion );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

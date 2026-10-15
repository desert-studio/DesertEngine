// P6: DynamicMesh3 reads and writes the scene's StaticMesh.EditMesh block (SavedMeshForm.hpp).
//
//   - on every tracked scene carrying the block, and on synthetic variants of it that exercise what the scene
//     does not (colours, a second UV layer, partly set layers, flipped handedness, mixed materials, an
//     enabled-but-empty normal layer), the round trip is byte-stable (the witness file itself is hand-spelled
//     in doubles that are not floats, so the first write narrows it);
//   - broken data is refused with the entity's name in the message.

#include <gtest/gtest.h>

#include "TestSupport/repo_root.hpp"

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>

#include <Common/Json/Document.hpp>

#include <optional>

namespace
{
    // A block read at the document root: the Issues are what a refusal would have reported.
    template <class T>
    std::optional<T> ReadBlockOf( const Common::Json::Value& value )
    {
        Common::Json::Issues issues;
        return Desert::Core::Serialize::ReadBlock<T>( Common::Json::Root( value ), issues );
    }
} // namespace
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>

#include <glm/geometric.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Geometry;
using Desert::Assets::StaticMeshComponentSer;

namespace
{

    struct CorpusMesh
    {
        std::string Name; // "<scene file>:<entity tag>"
        MeshSavedForm Saved;
    };

    // Every tracked scene with a StaticMesh.EditMesh block. Discovered, not listed, so a scene added later is
    // covered without touching this file; the witness scene is pinned by name in the test below.
    std::vector<CorpusMesh> Corpus()
    {
        std::vector<CorpusMesh> out;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator(
                   Desert::TestSupport::RepoRoot() / "Projects/Desert/Content/Scenes" ) )
        {
            if ( entry.path().extension() != ".desce" )
                continue;
            std::ifstream     file( entry.path(), std::ios::binary );
            std::stringstream text;
            text << file.rdbuf();
            if ( text.str().find( "\"EditMesh\"" ) == std::string::npos )
                continue;
            const auto scene = Common::Json::Parse( text.str() );
            EXPECT_TRUE( scene.IsSuccess() ) << entry.path();
            if ( !scene )
                continue;
            const auto entities = Common::Json::Root( scene.GetValue() ).Find( "Entities" );
            if ( !entities )
                continue;
            entities->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& entity )
                 {
                     const auto staticMesh = entity.Find( "StaticMesh" );
                     if ( !staticMesh || !staticMesh->Find( "EditMesh" ) )
                         return;
                     std::string tag = "?";
                     if ( const auto tagNode = entity.Find( "Tag" ) )
                         if ( const auto tagText = tagNode->AsString() )
                             tag = tagText.GetValue();
                     const auto block = ReadBlockOf<StaticMeshComponentSer>( staticMesh->Raw() );
                     EXPECT_TRUE( block && block->EditMesh ) << entry.path();
                     if ( !block || !block->EditMesh )
                         return;
                     out.push_back( { entry.path().filename().string() + ":" + tag, *block->EditMesh } );
                 } );
        }
        return out;
    }

    // The scene's own write of the block, as text: what "byte for byte" is measured on.
    std::string Written( const MeshSavedForm& saved )
    {
        StaticMeshComponentSer block;
        block.EditMesh = saved;
        return Common::Json::Write( Common::Json::FromStruct( block ) );
    }

    DynamicMesh3 NewRead( const MeshSavedForm& saved, const std::string& name )
    {
        auto mesh = DynamicMeshFromSerialized( saved, name );
        EXPECT_TRUE( mesh.IsSuccess() ) << ( mesh.IsSuccess() ? "" : mesh.GetError() );
        return mesh.IsSuccess() ? mesh.ExtractValue() : DynamicMesh3{};
    }

    // Per-vertex elements for a new layer: element v = the values of vertex v, every triangle set except
    // the rows in @p unsetRows.
    MeshOverlaySavedForm PerVertex( const MeshSavedForm& mesh, int components, std::vector<size_t> unsetRows )
    {
        MeshOverlaySavedForm out;
        const size_t       vertices = mesh.Positions.size() / 3;
        for ( size_t v = 0; v < vertices; ++v )
            for ( int c = 0; c < components; ++c )
                out.Values.push_back( 0.125f * static_cast<float>( v ) + 0.5f * static_cast<float>( c ) );
        out.Triangles = mesh.Triangles;
        for ( const size_t row : unsetRows )
            for ( int j = 0; j < 3; ++j )
                out.Triangles[row * 3 + j] = -1;
        return out;
    }

    // What the corpus does not exercise, derived from a corpus mesh so the topology stays a real one.
    std::vector<CorpusMesh> Variants( const CorpusMesh& base )
    {
        std::vector<CorpusMesh> out;
        const size_t            triangles = base.Saved.Triangles.size() / 3;

        CorpusMesh layered = base;
        layered.Name += " +colors +UV1 partly set, mixed materials and groups";
        layered.Saved.Colors = PerVertex( base.Saved, 4, { 2 } );
        layered.Saved.UVs.push_back( PerVertex( base.Saved, 2, { 0, 1 } ) );
        for ( size_t t = 0; t < triangles; ++t )
        {
            layered.Saved.MaterialIds[t] = t < triangles / 2 ? 3 : 7;
            layered.Saved.PolyGroups[t]  = 10 + static_cast<int>( ( t * 7 ) % 3 );
        }
        out.push_back( layered );

        CorpusMesh flipped = base;
        flipped.Name += " handedness flipped";
        if ( flipped.Saved.Tangents )
            for ( size_t i = 3; i < flipped.Saved.Tangents->Values.size(); i += 4 )
                flipped.Saved.Tangents->Values[i] = -flipped.Saved.Tangents->Values[i];
        out.push_back( flipped );

        CorpusMesh bare = base;
        bare.Name += " normals enabled but empty, no tangents, no UVs";
        bare.Saved.Tangents.reset();
        bare.Saved.UVs.clear();
        bare.Saved.Normals = MeshOverlaySavedForm{ {}, std::vector<int>( base.Saved.Triangles.size(), -1 ) };
        out.push_back( bare );

        CorpusMesh none = base;
        none.Name += " no layers at all";
        none.Saved.Tangents.reset();
        none.Saved.Normals.reset();
        none.Saved.UVs.clear();
        out.push_back( none );
        return out;
    }

    const std::vector<CorpusMesh>& CorpusWithVariants()
    {
        static const std::vector<CorpusMesh> all = []
        {
            std::vector<CorpusMesh> meshes = Corpus();
            const size_t            files  = meshes.size();
            for ( size_t i = 0; i < files; ++i )
                for ( CorpusMesh& v : Variants( meshes[i] ) )
                    meshes.push_back( std::move( v ) );
            return meshes;
        }();
        return all;
    }

    MeshSavedForm Witness()
    {
        for ( const CorpusMesh& mesh : CorpusWithVariants() )
            if ( mesh.Name == "M4_RampNormalMap.desce:Ramp_EditMesh" )
                return mesh.Saved;
        ADD_FAILURE() << "the witness scene M4_RampNormalMap.desce:Ramp_EditMesh is not in the corpus";
        return {};
    }

    std::string RefusalOf( const MeshSavedForm& saved )
    {
        auto mesh = DynamicMeshFromSerialized( saved, "Broken_Entity" );
        EXPECT_FALSE( mesh.IsSuccess() );
        if ( mesh.IsSuccess() )
            return {};
        EXPECT_NE( mesh.GetError().find( "Broken_Entity" ), std::string::npos ) << mesh.GetError();
        return mesh.GetError();
    }
} // namespace

TEST( DynamicMeshSerialization, CorpusHoldsTheWitnessScene )
{
    std::printf( "corpus: %zu meshes with variants\n", CorpusWithVariants().size() );
    for ( const CorpusMesh& mesh : CorpusWithVariants() )
        std::printf( "  %s\n", mesh.Name.c_str() );
    const MeshSavedForm witness = Witness();
    EXPECT_EQ( witness.Triangles.size(), 36u );
    ASSERT_TRUE( witness.Tangents.has_value() );
    // the witness carries both handedness signs, so the sign recovery is exercised by the scene itself
    bool negative = false, positive = false;
    for ( size_t i = 3; i < witness.Tangents->Values.size(); i += 4 )
        ( witness.Tangents->Values[i] < 0.0f ? negative : positive ) = true;
    EXPECT_TRUE( negative && positive );
}

TEST( DynamicMeshSerialization, RoundTripIsByteStable )
{
    for ( const CorpusMesh& mesh : CorpusWithVariants() )
    {
        const std::string first = Written( ToSerialized( NewRead( mesh.Saved, mesh.Name ) ) );
        const auto        reparsed = Common::Json::Parse( first );
        ASSERT_TRUE( reparsed.IsSuccess() ) << reparsed.GetError();
        const auto again = ReadBlockOf<StaticMeshComponentSer>( reparsed.GetValue() );
        ASSERT_TRUE( again && again->EditMesh ) << mesh.Name;
        EXPECT_EQ( Written( ToSerialized( NewRead( *again->EditMesh, mesh.Name ) ) ), first ) << mesh.Name;
    }
}

TEST( DynamicMeshSerialization, TheSceneAndAVariantDraw )
{
    int drawn = 0;
    for ( const CorpusMesh& mesh : CorpusWithVariants() )
        drawn += ToRenderMesh( NewRead( mesh.Saved, mesh.Name ) ).IsSuccess();
    // The scene and one variant draw. The layer-less ones, and the one whose colour / UV1 layers are only partly
    // set (a drawn stream has no value for an unset triangle), are refused.
    EXPECT_GE( drawn, 2 );
}

TEST( DynamicMeshSerialization, HolesAreCompactedOnWrite )
{
    DynamicMesh3 mesh = NewRead( Witness(), "witness" );
    ASSERT_EQ( mesh.RemoveTriangle( 3, false ), MeshResult::Ok );
    const MeshSavedForm saved = ToSerialized( mesh );
    EXPECT_EQ( saved.Triangles.size(), 33u );
    EXPECT_EQ( saved.PolyGroups.size(), 11u );
    const DynamicMesh3 back = NewRead( saved, "witness" );
    EXPECT_EQ( back.TriangleCount(), 11 );
    EXPECT_EQ( Written( ToSerialized( back ) ), Written( saved ) );
}

TEST( DynamicMeshSerialization, BrokenDataIsRefusedByEntityName )
{
    const MeshSavedForm good = Witness();

    MeshSavedForm outOfRange = good;
    outOfRange.Triangles[4] = 99;
    EXPECT_NE( RefusalOf( outOfRange ).find( "names vertex 99 of 8" ), std::string::npos );

    // a third triangle on the edge (0, 1): the first two already share it
    MeshSavedForm nonManifold = good;
    nonManifold.Positions.insert( nonManifold.Positions.end(), { 0.0f, -300.0f, 0.0f } );
    nonManifold.Triangles.insert( nonManifold.Triangles.end(), { 0, 1, 8 } );
    nonManifold.PolyGroups.push_back( 0 );
    nonManifold.MaterialIds.push_back( 0 );
    for ( auto* layer : { &*nonManifold.Normals, &*nonManifold.Tangents, &nonManifold.UVs[0] } )
        layer->Triangles.insert( layer->Triangles.end(), { -1, -1, -1 } );
    EXPECT_NE( RefusalOf( nonManifold ).find( "non-manifold" ), std::string::npos );

    MeshSavedForm degenerate = good;
    degenerate.Triangles[1] = degenerate.Triangles[0];
    EXPECT_NE( RefusalOf( degenerate ).find( "degenerate" ), std::string::npos );

    MeshSavedForm elementOutOfRange         = good;
    elementOutOfRange.Normals->Triangles[0] = 1000;
    EXPECT_NE( RefusalOf( elementOutOfRange ).find( "normals: triangle 0 names element 1000" ),
               std::string::npos );

    // triangle 0 is (0, 1, 2) and triangle 1 is (2, 3, 0): point triangle 1's corner at vertex 3 at the element
    // triangle 0 uses at vertex 1
    MeshSavedForm sharedElement         = good;
    sharedElement.Normals->Triangles[4] = sharedElement.Normals->Triangles[1];
    EXPECT_NE( RefusalOf( sharedElement ).find( "is used at vertex" ), std::string::npos );

    MeshSavedForm partly       = good;
    partly.UVs[0].Triangles[5] = -1;
    EXPECT_NE( RefusalOf( partly ).find( "only partly set" ), std::string::npos );

    MeshSavedForm unused = good;
    unused.Normals->Values.insert( unused.Normals->Values.end(), { 0.0f, 1.0f, 0.0f } );
    EXPECT_NE( RefusalOf( unused ).find( "is used by no triangle" ), std::string::npos );

    MeshSavedForm halfSign       = good;
    halfSign.Tangents->Values[3] = 0.5f;
    EXPECT_NE( RefusalOf( halfSign ).find( "handedness 0.5" ), std::string::npos );

    MeshSavedForm tangentOnly = good;
    tangentOnly.Normals.reset();
    EXPECT_NE( RefusalOf( tangentOnly ).find( "without a normal layer" ), std::string::npos );

    MeshSavedForm ragged = good;
    ragged.Positions.pop_back();
    EXPECT_NE( RefusalOf( ragged ).find( "not a whole number of vertices" ), std::string::npos );
}

// FDYNAMICMESH3 AS A .stmesh AND BACK (P7).
//
// The reference is the EditMesh pair (EditMeshAsset.hpp). The DynamicMesh3 under test is NOT built from the
// same render arrays the EditMesh path writes (that would compare a function with itself): it is read from
// the EditMesh's scene block through the P6 reader, the independent road a scene mesh takes into the new core.
// Meshes: the six generator shapes (with polygroups and two materials) and every tracked scene mesh.

#include <gtest/gtest.h>

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
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
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshAsset.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Geometry;
namespace Ser = Desert::Assets::Serialization;

namespace
{
    const std::vector<Common::Content::AssetGuid> kSlots = { { 1111, 1 }, { 2222, 2 }, { 3333, 3 } };

    struct Case
    {
        std::string Name;
        EditMesh    Mesh;
    };

    // Odd polygroups on material `odd`, the rest on 0: two submeshes, and with odd = 2 a gap the file
    // compacts away.
    EditMesh Shape( const Common::ResultStr<EditMesh>& made, int odd )
    {
        EXPECT_TRUE( made.IsSuccess() ) << ( made.IsSuccess() ? "" : made.GetError() );
        EditMesh mesh = made.IsSuccess() ? made.GetValue() : EditMesh{};
        for ( const int t : mesh.TriangleIds() )
            mesh.Attributes().SetMaterialId( t, mesh.Attributes().GetPolyGroup( t ) % 2 == 1 ? odd : 0 );
        return mesh;
    }

    // Every tracked scene mesh, discovered (as DynamicMeshSerialization discovers them).
    void AddScenes( std::vector<Case>& out )
    {
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( DESERT_SCENES_DIR ) )
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
                     const auto block = ReadBlockOf<Assets::StaticMeshComponentSer>( staticMesh->Raw() );
                     if ( !block || !block->EditMesh )
                         return;
                     auto mesh = FromSerialized( *block->EditMesh );
                     EXPECT_TRUE( mesh.IsSuccess() ) << entry.path();
                     if ( mesh.IsSuccess() )
                         out.push_back( { entry.path().filename().string() + ":" + tag, mesh.ExtractValue() } );
                 } );
        }
    }

    const std::vector<Case>& Cases()
    {
        static const std::vector<Case> all = []
        {
            std::vector<Case> out;
            out.push_back(
                 { "box",
                   Shape( ShapeToEditMesh( MakeBox( glm::vec3( 200, 100, 50 ), glm::ivec3( 2, 1, 1 ) ) ), 1 ) } );
            out.push_back( { "sphere", Shape( ShapeToEditMesh( MakeSphere( 100.0f ) ), 2 ) } );
            out.push_back( { "cylinder", Shape( ShapeToEditMesh( MakeCylinder( 80.0f, 150.0f ) ), 1 ) } );
            out.push_back( { "cone", Shape( ShapeToEditMesh( MakeCone( 80.0f, 150.0f ) ), 2 ) } );
            out.push_back( { "capsule", Shape( ShapeToEditMesh( MakeCapsule( 60.0f, 180.0f ) ), 1 ) } );
            out.push_back(
                 { "pyramid", Shape( ShapeToEditMesh( MakePyramid( glm::vec3( 100, 120, 100 ) ) ), 1 ) } );
            AddScenes( out );
            return out;
        }();
        return all;
    }

    // The new core's mesh, reached through the scene block (P6), not through the render arrays.
    DynamicMesh3 NewCore( const Case& c )
    {
        auto mesh = DynamicMeshFromSerialized( ToSerialized( c.Mesh ), c.Name );
        EXPECT_TRUE( mesh.IsSuccess() ) << c.Name << ": " << ( mesh.IsSuccess() ? "" : mesh.GetError() );
        return mesh.IsSuccess() ? mesh.ExtractValue() : DynamicMesh3{};
    }

    std::string Bytes( const Common::ResultStr<Ser::MeshAssetData>& data, const std::string& name )
    {
        EXPECT_TRUE( data.IsSuccess() ) << name << ": " << ( data.IsSuccess() ? "" : data.GetError() );
        return data.IsSuccess() ? Ser::EncodeMeshBinary( data.GetValue() ) : std::string{};
    }

    Ser::MeshAssetData Decoded( const std::string& bytes )
    {
        auto data = Ser::DecodeMeshBinary( bytes, "DynamicMeshAsset" );
        EXPECT_TRUE( data.IsSuccess() ) << ( data.IsSuccess() ? "" : data.GetError() );
        return data.IsSuccess() ? data.ExtractValue() : Ser::MeshAssetData{};
    }

    // Where two files part, for the failure message: the first differing array and index.
    std::string FirstDifference( const std::string& a, const std::string& b )
    {
        const Ser::MeshAssetData x = Decoded( a ), y = Decoded( b );
        if ( x.StaticVertices.size() != y.StaticVertices.size() )
            return "vertex count " + std::to_string( x.StaticVertices.size() ) + " vs " +
                   std::to_string( y.StaticVertices.size() );
        for ( size_t i = 0; i < x.StaticVertices.size(); ++i )
        {
            const auto& p = x.StaticVertices[i];
            const auto& q = y.StaticVertices[i];
            // bitwise: a -0 that came back +0 is a different file
            const auto differs = []( const auto& l, const auto& r )
            { return std::memcmp( &l, &r, sizeof( l ) ) != 0; };
            const char* what = differs( p.Position, q.Position )     ? "position"
                               : differs( p.Normal, q.Normal )       ? "normal"
                               : differs( p.Tangent, q.Tangent )     ? "tangent"
                               : differs( p.Bitangent, q.Bitangent ) ? "bitangent"
                               : differs( p.TexCoord, q.TexCoord )   ? "uv"
                                                                     : nullptr;
            if ( what )
            {
                const auto field = [&]( const Ser::StaticVertexData& v ) -> const float*
                {
                    return what[0] == 'p'   ? &v.Position.x
                           : what[0] == 'n' ? &v.Normal.x
                           : what[0] == 't' ? &v.Tangent.x
                           : what[0] == 'b' ? &v.Bitangent.x
                                            : &v.TexCoord.x;
                };
                const float* l = field( p );
                const float* r = field( q );
                char         text[160];
                std::snprintf( text, sizeof( text ), " (%g %g %g) vs (%g %g %g)", l[0], l[1],
                               what[0] == 'u' ? 0.f : l[2], r[0], r[1], what[0] == 'u' ? 0.f : r[2] );
                return std::string( what ) + " of vertex " + std::to_string( i ) + text;
            }
        }
        for ( size_t i = 0; i < x.Indices.size() && i < y.Indices.size(); ++i )
            if ( x.Indices[i].V1 != y.Indices[i].V1 || x.Indices[i].V2 != y.Indices[i].V2 ||
                 x.Indices[i].V3 != y.Indices[i].V3 )
                return "face " + std::to_string( i );
        for ( size_t i = 0; i < x.PolyGroups.size() && i < y.PolyGroups.size(); ++i )
            if ( x.PolyGroups[i] != y.PolyGroups[i] )
                return "polygroup of face " + std::to_string( i );
        for ( size_t i = 0; i < x.Submeshes.size() && i < y.Submeshes.size(); ++i )
        {
            const auto& p = x.Submeshes[i];
            const auto& q = y.Submeshes[i];
            if ( p.Name != q.Name )
                return "name of submesh " + std::to_string( i ) + ": " + p.Name + " vs " + q.Name;
            if ( p.VertexOffset != q.VertexOffset || p.VertexCount != q.VertexCount ||
                 p.IndexOffset != q.IndexOffset || p.IndexCount != q.IndexCount )
                return "ranges of submesh " + std::to_string( i );
            if ( p.BoundingBox.Min != q.BoundingBox.Min || p.BoundingBox.Max != q.BoundingBox.Max )
                return "box of submesh " + std::to_string( i );
        }
        return "sizes: faces " + std::to_string( x.Indices.size() ) + " vs " + std::to_string( y.Indices.size() ) +
               ", groups " + std::to_string( x.PolyGroups.size() ) + " vs " +
               std::to_string( y.PolyGroups.size() ) + ", submeshes " + std::to_string( x.Submeshes.size() ) +
               " vs " + std::to_string( y.Submeshes.size() );
    }

    Ser::MeshAssetData OneBoxFile()
    {
        auto data = DynamicMeshToMeshAssetData( NewCore( Cases().front() ), kSlots );
        EXPECT_TRUE( data.IsSuccess() );
        return data.IsSuccess() ? data.ExtractValue() : Ser::MeshAssetData{};
    }

    std::string RefusalOf( const Common::ResultStr<DynamicMesh3>& r )
    {
        EXPECT_FALSE( r.IsSuccess() );
        return r.IsSuccess() ? std::string{} : r.GetError();
    }

    std::string RefusalOf( const Common::ResultStr<Ser::MeshAssetData>& r )
    {
        EXPECT_FALSE( r.IsSuccess() );
        return r.IsSuccess() ? std::string{} : r.GetError();
    }
} // namespace

TEST( DynamicMeshAsset, CorpusHoldsShapesAndSceneMeshes )
{
    std::printf( "corpus: %zu meshes\n", Cases().size() );
    for ( const Case& c : Cases() )
        std::printf( "  %s: %d triangles\n", c.Name.c_str(), c.Mesh.TriangleCount() );
    EXPECT_GT( Cases().size(), 6u ) << "no tracked scene mesh was found";
}

TEST( DynamicMeshAsset, BothCoresWriteTheSameBytes )
{
    for ( const Case& c : Cases() )
    {
        const std::string before = Bytes( ToMeshAssetData( c.Mesh, kSlots ), c.Name );
        const std::string after  = Bytes( DynamicMeshToMeshAssetData( NewCore( c ), kSlots ), c.Name );
        EXPECT_FALSE( before.empty() ) << c.Name;
        EXPECT_EQ( before.size(), after.size() ) << c.Name;
        EXPECT_TRUE( before == after ) << c.Name << ": the two cores wrote different .stmesh bytes";
    }
}

TEST( DynamicMeshAsset, FileToMeshToFileIsByteStable )
{
    for ( const Case& c : Cases() )
    {
        const std::string first = Bytes( DynamicMeshToMeshAssetData( NewCore( c ), kSlots ), c.Name );
        auto              read  = DynamicMeshFromMeshAssetData( Decoded( first ) );
        ASSERT_TRUE( read.IsSuccess() ) << c.Name << ": " << read.GetError();
        EXPECT_TRUE(
             read.GetValue().CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::Check ) )
             << c.Name;
        const std::string second = Bytes( DynamicMeshToMeshAssetData( read.GetValue(), kSlots ), c.Name );
        // THE ONE BOUNDED DIFFERENCE, shared with the EditMesh reader: a mesh whose MaterialIDs have gaps is read
        // back compacted (MaterialID = submesh index), so its submesh NAMES ("MaterialID <id>") change once and
        // nothing else does; the second trip is then byte-stable. Every other mesh is stable at once.
        Ser::MeshAssetData       renamed = Decoded( first );
        const Ser::MeshAssetData again   = Decoded( second );
        ASSERT_EQ( renamed.Submeshes.size(), again.Submeshes.size() ) << c.Name;
        bool gaps = false;
        for ( size_t k = 0; k < renamed.Submeshes.size(); ++k )
        {
            gaps                      = gaps || renamed.Submeshes[k].Name != "MaterialID " + std::to_string( k );
            renamed.Submeshes[k].Name = again.Submeshes[k].Name;
        }
        EXPECT_TRUE( Ser::EncodeMeshBinary( renamed ) == second )
             << c.Name << ": the file read into the new core wrote back different bytes, first at "
             << FirstDifference( first, second );
        EXPECT_EQ( first == second, !gaps ) << c.Name << ": only a mesh with MaterialID gaps may rename submeshes";
        auto third = DynamicMeshFromMeshAssetData( again );
        ASSERT_TRUE( third.IsSuccess() ) << c.Name;
        EXPECT_TRUE( Bytes( DynamicMeshToMeshAssetData( third.GetValue(), kSlots ), c.Name ) == second )
             << c.Name << ": the compacted file is not a fixed point";

        // and the old core reads the new core's file to the same bytes the new core does
        auto old = FromMeshAssetData( Decoded( first ) );
        ASSERT_TRUE( old.IsSuccess() ) << c.Name << ": " << old.GetError();
        const std::string oldBytes = Bytes( ToMeshAssetData( old.GetValue(), kSlots ), c.Name );
        EXPECT_TRUE( oldBytes == second ) << c.Name << ": the old core read the file to other bytes, first at "
                                          << FirstDifference( second, oldBytes );
    }
}

TEST( DynamicMeshAsset, PolygroupsAndMaterialsSurviveTheFile )
{
    for ( const Case& c : Cases() )
    {
        const DynamicMesh3  mesh = NewCore( c );
        auto                data = DynamicMeshToMeshAssetData( mesh, kSlots );
        ASSERT_TRUE( data.IsSuccess() ) << c.Name << ": " << data.GetError();
        const Ser::MeshAssetData file = Decoded( Ser::EncodeMeshBinary( data.GetValue() ) );
        ASSERT_EQ( file.PolyGroups.size(), file.Indices.size() ) << c.Name;
        for ( size_t k = 0; k < file.Submeshes.size(); ++k )
            EXPECT_EQ( file.Submeshes[k].MaterialGuid, kSlots[k] ) << c.Name << " submesh " << k;

        auto read = DynamicMeshFromMeshAssetData( file );
        ASSERT_TRUE( read.IsSuccess() ) << c.Name << ": " << read.GetError();
        const DynamicMesh3& back = read.GetValue();
        ASSERT_EQ( back.TriangleCount(), mesh.TriangleCount() ) << c.Name;
        ASSERT_EQ( back.VertexCount(), mesh.VertexCount() ) << c.Name << ": the weld did not close the shell";

        // Face k of the file came from triangle SourceTriangles[k] and is triangle k of the mesh read back.
        auto render = ToRenderMesh( mesh );
        ASSERT_TRUE( render.IsSuccess() );
        const std::vector<int>& source = render.GetValue().SourceTriangles;
        const auto*             before = mesh.Attributes()->GetMaterialID();
        const auto*             after  = back.Attributes()->GetMaterialID();
        std::set<int>           groups, materials;
        std::map<int, int>      slotOf; // original MaterialID -> submesh index it lands on
        for ( size_t k = 0; k < source.size(); ++k )
        {
            const int t = static_cast<int>( k );
            EXPECT_EQ( back.GetTriangleGroup( t ), mesh.GetTriangleGroup( source[k] ) ) << c.Name << " face " << k;
            const int material     = before->GetValue( source[k] );
            const auto [it, fresh] = slotOf.emplace( material, after->GetValue( t ) );
            EXPECT_EQ( it->second, after->GetValue( t ) ) << c.Name << " face " << k;
            groups.insert( back.GetTriangleGroup( t ) );
            materials.insert( after->GetValue( t ) );
        }
        // compaction keeps the order: MaterialIDs ascending map onto submeshes 0..N-1
        int expected = 0;
        for ( const auto& [material, slot] : slotOf )
            EXPECT_EQ( slot, expected++ ) << c.Name << " MaterialID " << material;
        if ( c.Name == "box" )
        {
            EXPECT_EQ( groups.size(), 6u ) << "the box carries one group per face";
            EXPECT_EQ( materials.size(), 2u );
        }
        if ( c.Name == "cone" )
            EXPECT_EQ( slotOf.count( 2 ), 1u ) << "the cone's MaterialID 2 should exist and compact to 1";
    }
}

TEST( DynamicMeshAsset, AFileWithoutGroupsReadsWithEveryFaceInGroupZero )
{
    Ser::MeshAssetData file = OneBoxFile();
    file.PolyGroups.clear();
    auto read = DynamicMeshFromMeshAssetData( file );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    ASSERT_TRUE( read.GetValue().HasTriangleGroups() );
    for ( const int t : read.GetValue().TriangleIndicesItr() )
        EXPECT_EQ( read.GetValue().GetTriangleGroup( t ), 0 ) << "triangle " << t;
}

TEST( DynamicMeshAsset, WhatTheFileCannotHoldIsRefusedByName )
{
    const DynamicMesh3 base = NewCore( Cases().front() );

    DynamicMesh3 colours = base;
    colours.Attributes()->EnablePrimaryColors();
    EXPECT_NE( RefusalOf( DynamicMeshToMeshAssetData( colours, kSlots ) ).find( "colour" ), std::string::npos );

    DynamicMesh3 uvs = base;
    uvs.Attributes()->SetNumUVLayers( 2 );
    EXPECT_NE( RefusalOf( DynamicMeshToMeshAssetData( uvs, kSlots ) ).find( "2 UV layers" ), std::string::npos );

    DynamicMesh3 layers = base;
    layers.Attributes()->SetNumPolygroupLayers( 1 );
    EXPECT_NE( RefusalOf( DynamicMeshToMeshAssetData( layers, kSlots ) ).find( "polygroup layer" ),
               std::string::npos );

    EXPECT_NE( RefusalOf( DynamicMeshToMeshAssetData( DynamicMesh3{}, kSlots ) ).find( "no triangle" ),
               std::string::npos );

    // the same meshes the EditMesh reader refuses
    Ser::MeshAssetData skinned = OneBoxFile();
    skinned.IsSkinned          = true;
    EXPECT_NE( RefusalOf( DynamicMeshFromMeshAssetData( skinned ) ).find( "skinned" ), std::string::npos );

    Ser::MeshAssetData short_ = OneBoxFile();
    short_.PolyGroups.pop_back();
    EXPECT_NE( RefusalOf( DynamicMeshFromMeshAssetData( short_ ) ).find( "polygroup entries" ),
               std::string::npos );

    // a face repeated: it would weld onto an existing triangle and shift every group after it
    Ser::MeshAssetData duplicate = OneBoxFile();
    duplicate.Indices.push_back( duplicate.Indices.back() );
    duplicate.PolyGroups.push_back( 0 );
    duplicate.Submeshes.back().IndexCount += 3;
    const std::string refusal = RefusalOf( DynamicMeshFromMeshAssetData( duplicate ) );
    EXPECT_NE( refusal.find( "1 duplicate" ), std::string::npos ) << refusal;
    EXPECT_FALSE( FromMeshAssetData( duplicate ).IsSuccess() ) << "the two cores disagree on what they open";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

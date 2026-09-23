// THE EDITED MESH SURVIVES A SAVE: what a scene stores for a mesh built in the editor, and that the mesh which
// comes back is the one that went in (M4).
//
// Before schema v22 the StaticMesh block stored the RENDER buffer, and only Position/Normal/TexCoord of it.
// Three things were lost on every save and nothing said so: the tangent frame (the loader left it
// uninitialised), the submeshes (all folded into one "Mesh"), and everything the render side cannot carry at
// all - topology, polygroups, colours, extra UV layers. The block now stores the EditMesh itself.
//
// THE ROUND TRIP HERE IS THE SCENE'S OWN: StaticMeshComponentSer written to JSON text and read back through
// the same WriteBlock/ReadBlock ComponentRegistry calls, then Geometry::FromSerialized - the exact sequence a
// save followed by a load performs. ComponentRegistry.cpp itself cannot be linked here (it pulls the asset
// manager and the device); the two lines it adds around this are ToSerialized and SetEditableMesh.
//
// THE RELATION ASSERTED IS "THE SAME MESH", checked two ways: the saved forms of the two meshes are equal
// (topology, polygroups, material IDs, every overlay element and every triangle's elements), AND the render
// meshes derived from them are equal byte for byte - vertices with their tangent frames, indices, submeshes
// and each submesh's material ID. The second is what the screen sees; the first is what the next edit sees.

#include <gtest/gtest.h>

#include "../EditMesh/EditMeshTestSupport.hpp"

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace EditMeshTest;
    using Desert::Assets::StaticMeshComponentSer;
    using Desert::Core::Serialize::ReadBlock;
    using Desert::Core::Serialize::WriteBlock;
    using namespace Desert::Geometry;

    // An octahedron carrying EVERY kind of state the saved form holds, each in a shape that a lossy writer
    // would get wrong:
    //   * normals hard per face (a seam on every edge) - a writer that stored one normal per vertex merges them;
    //   * tangents smooth per vertex, NOT perpendicular to the normal and with both signs - a writer that
    //     recomputed the frame, or dropped w, changes them (CubeGrid's ramps store exactly such a tangent);
    //   * two materials, not numbered 0..1 - the render side has two submeshes, one per material;
    //   * polygroups that are neither the material nor the triangle ID;
    //   * a colour layer and a second UV layer, which the render side cannot carry at all;
    //   * a HOLE: one triangle removed, so the IDs are not dense and the writer has to renumber.
    EditMesh MakeRichMesh()
    {
        EditMesh mesh = MakeOctahedron();
        auto&    attr = mesh.Attributes();

        attr.EnableNormals();
        attr.EnableTangents();
        attr.EnableColors();
        EXPECT_TRUE( attr.SetUVLayerCount( 2 ) );

        std::vector<int> tangentOf( mesh.MaxVertexId() ), colorOf( mesh.MaxVertexId() ), uv1Of( mesh.MaxVertexId() );
        for ( const int v : mesh.VertexIds() )
        {
            const glm::vec3 p = mesh.GetPosition( v );
            tangentOf[v] = attr.Tangents()->AppendElement( { 0.6f, 0.8f, 0.0f, v % 2 == 0 ? 1.0f : -1.0f } );
            colorOf[v]   = attr.Colors()->AppendElement( { p.x / 100.0f, p.y / 100.0f, p.z / 100.0f, 0.5f } );
            uv1Of[v]     = attr.UV( 1 )->AppendElement( { p.x * 0.013f, p.z * 0.017f } );
        }

        for ( const int t : mesh.TriangleIds() )
        {
            const auto&     tri = mesh.GetTriangle( t );
            const glm::vec3 n   = glm::normalize( glm::cross( mesh.GetPosition( tri[1] ) - mesh.GetPosition( tri[0] ),
                                                              mesh.GetPosition( tri[2] ) - mesh.GetPosition( tri[0] ) ) );
            const int       en  = attr.Normals()->AppendElement( n );
            EXPECT_EQ( attr.Normals()->SetTriangle( mesh, t, { en, en, en } ), EditResult::Ok );
            EXPECT_EQ( attr.Tangents()->SetTriangle( mesh, t, { tangentOf[tri[0]], tangentOf[tri[1]], tangentOf[tri[2]] } ),
                       EditResult::Ok );
            EXPECT_EQ( attr.Colors()->SetTriangle( mesh, t, { colorOf[tri[0]], colorOf[tri[1]], colorOf[tri[2]] } ),
                       EditResult::Ok );
            // UV 0: its own three elements per triangle (a seam on every edge, like a per-face projection).
            std::array<int, 3> uv0{};
            for ( int j = 0; j < 3; ++j )
                uv0[j] = attr.UV( 0 )->AppendElement( { 0.25f * j + 0.01f * t, 0.5f - 0.03f * t } );
            EXPECT_EQ( attr.UV( 0 )->SetTriangle( mesh, t, uv0 ), EditResult::Ok );
            EXPECT_EQ( attr.UV( 1 )->SetTriangle( mesh, t, { uv1Of[tri[0]], uv1Of[tri[1]], uv1Of[tri[2]] } ),
                       EditResult::Ok );
            attr.SetPolyGroup( t, 10 + ( t * 7 ) % 3 );
            attr.SetMaterialId( t, t < 4 ? 3 : 7 );
        }

        EXPECT_EQ( mesh.RemoveTriangle( 5 ), EditResult::Ok );
        EXPECT_TRUE( Valid( mesh ) );
        return mesh;
    }

    // The save and the load, exactly as ComponentRegistry performs them.
    EditMesh SaveAndLoad( const EditMesh& mesh )
    {
        StaticMeshComponentSer written;
        written.EditMesh    = ToSerialized( mesh );
        written.CastShadows = false; // a neighbour field, so the block is not the mesh alone

        const auto read = ReadBlock<StaticMeshComponentSer>( WriteBlock( written, "StaticMesh" ), "StaticMesh" );
        EXPECT_TRUE( read.has_value() );
        EXPECT_TRUE( read.has_value() && read->EditMesh.has_value() );
        if ( !read || !read->EditMesh )
            return {};
        auto loaded = FromSerialized( *read->EditMesh );
        EXPECT_TRUE( loaded.IsSuccess() ) << ( loaded.IsSuccess() ? "" : loaded.GetError() );
        return loaded.IsSuccess() ? loaded.ExtractValue() : EditMesh{};
    }

    RenderMeshData Render( const EditMesh& mesh )
    {
        auto render = ToRenderMesh( mesh );
        EXPECT_TRUE( render.IsSuccess() ) << ( render.IsSuccess() ? "" : render.GetError() );
        return render.IsSuccess() ? render.ExtractValue() : RenderMeshData{};
    }

    ::testing::AssertionResult SameRenderMesh( const RenderMeshData& a, const RenderMeshData& b )
    {
        if ( a.Vertices.size() != b.Vertices.size() )
            return ::testing::AssertionFailure() << a.Vertices.size() << " vs " << b.Vertices.size() << " vertices";
        for ( size_t i = 0; i < a.Vertices.size(); ++i )
            if ( std::memcmp( &a.Vertices[i], &b.Vertices[i], sizeof( Desert::Vertex ) ) != 0 )
                return ::testing::AssertionFailure() << "render vertex " << i << " differs";
        if ( a.Indices.size() != b.Indices.size() )
            return ::testing::AssertionFailure() << a.Indices.size() << " vs " << b.Indices.size() << " triangles";
        for ( size_t i = 0; i < a.Indices.size(); ++i )
            if ( a.Indices[i].V1 != b.Indices[i].V1 || a.Indices[i].V2 != b.Indices[i].V2 ||
                 a.Indices[i].V3 != b.Indices[i].V3 )
                return ::testing::AssertionFailure() << "render triangle " << i << " differs";
        if ( a.Submeshes.size() != b.Submeshes.size() )
            return ::testing::AssertionFailure() << a.Submeshes.size() << " vs " << b.Submeshes.size() << " submeshes";
        for ( size_t i = 0; i < a.Submeshes.size(); ++i )
        {
            const auto &sa = a.Submeshes[i], &sb = b.Submeshes[i];
            if ( sa.Name != sb.Name || sa.VertexOffset != sb.VertexOffset || sa.VertexCount != sb.VertexCount ||
                 sa.IndexOffset != sb.IndexOffset || sa.IndexCount != sb.IndexCount ||
                 sa.BoundingBox.Min != sb.BoundingBox.Min || sa.BoundingBox.Max != sb.BoundingBox.Max )
                return ::testing::AssertionFailure() << "submesh " << i << " (" << sa.Name << ") differs";
        }
        if ( a.SubmeshMaterialIds != b.SubmeshMaterialIds )
            return ::testing::AssertionFailure() << "submesh material IDs differ";
        return ::testing::AssertionSuccess();
    }
} // namespace

TEST( EditMeshSaved, TheMeshThatComesBackIsTheMeshThatWentIn )
{
    const EditMesh original = MakeRichMesh();
    const EditMesh loaded   = SaveAndLoad( original );

    EXPECT_TRUE( Valid( loaded ) );
    EXPECT_EQ( loaded.VertexCount(), original.VertexCount() );
    EXPECT_EQ( loaded.TriangleCount(), original.TriangleCount() );
    EXPECT_TRUE( ToSerialized( loaded ) == ToSerialized( original ) );
}

TEST( EditMeshSaved, TheRenderMeshDerivedAfterALoadIsByteIdentical )
{
    const EditMesh       original = MakeRichMesh();
    const RenderMeshData before   = Render( original );
    const RenderMeshData after    = Render( SaveAndLoad( original ) );

    // The fixture is not trivial on the axes the v21 form lost: two submeshes, and tangents that a
    // recomputation would not reproduce (0.6, 0.8, 0 is not perpendicular to most of the face normals).
    ASSERT_EQ( before.Submeshes.size(), 2u );
    EXPECT_EQ( before.SubmeshMaterialIds, ( std::vector<int>{ 3, 7 } ) );
    EXPECT_FLOAT_EQ( before.Vertices[0].Tangent.x, 0.6f );
    EXPECT_TRUE( SameRenderMesh( before, after ) );
}

TEST( EditMeshSaved, PolyGroupsAndMaterialIdsFollowTheirTriangles )
{
    const EditMesh original = MakeRichMesh();
    const EditMesh loaded   = SaveAndLoad( original );

    // Compaction renumbers the triangles after the hole (ID 5), in ascending order; each keeps its own
    // polygroup and material across that renumbering.
    std::vector<int> groups, materials;
    for ( const int t : original.TriangleIds() )
    {
        groups.push_back( original.Attributes().GetPolyGroup( t ) );
        materials.push_back( original.Attributes().GetMaterialId( t ) );
    }
    std::vector<int> loadedGroups, loadedMaterials;
    for ( const int t : loaded.TriangleIds() )
    {
        loadedGroups.push_back( loaded.Attributes().GetPolyGroup( t ) );
        loadedMaterials.push_back( loaded.Attributes().GetMaterialId( t ) );
    }
    EXPECT_EQ( loadedGroups, groups );
    EXPECT_EQ( loadedMaterials, materials );
}

TEST( EditMeshSaved, ADisabledLayerStaysDisabledAndAnEmptyOneStaysEmpty )
{
    EditMesh mesh = MakeOctahedron();
    mesh.Attributes().EnableNormals(); // enabled, every triangle UNSET
    EditMesh loaded = SaveAndLoad( mesh );

    ASSERT_NE( loaded.Attributes().Normals(), nullptr );
    EXPECT_EQ( loaded.Attributes().Normals()->ElementCount(), 0 );
    EXPECT_EQ( loaded.Attributes().Tangents(), nullptr );
    EXPECT_EQ( loaded.Attributes().Colors(), nullptr );
    EXPECT_EQ( loaded.Attributes().UVLayerCount(), 0 );
}

TEST( EditMeshSaved, ASavedFormNoWriterProducesIsRefusedByName )
{
    const EditMeshSer good = ToSerialized( MakeRichMesh() );
    const auto        refusal = []( const EditMeshSer& saved )
    {
        const auto result = FromSerialized( saved );
        return result.IsSuccess() ? std::string( "<accepted>" ) : result.GetError();
    };

    EditMeshSer truncated = good;
    truncated.Positions.pop_back();
    EXPECT_NE( refusal( truncated ).find( "position floats" ), std::string::npos ) << refusal( truncated );

    EditMeshSer outOfRange = good;
    outOfRange.Triangles[4] = 99;
    EXPECT_NE( refusal( outOfRange ).find( "names vertex 99" ), std::string::npos ) << refusal( outOfRange );

    EditMeshSer partlySet = good;
    partlySet.Normals->Triangles[1] = -1;
    EXPECT_NE( refusal( partlySet ).find( "only partly set" ), std::string::npos ) << refusal( partlySet );

    EditMeshSer unusedElement = good;
    unusedElement.Colors->Values.insert( unusedElement.Colors->Values.end(), { 1.0f, 0.0f, 0.0f, 1.0f } );
    EXPECT_NE( refusal( unusedElement ).find( "not valid" ), std::string::npos ) << refusal( unusedElement );

    EditMeshSer duplicate = good;
    duplicate.Triangles.insert( duplicate.Triangles.end(), { good.Triangles[0], good.Triangles[1], good.Triangles[2] } );
    duplicate.PolyGroups.push_back( 0 );
    duplicate.MaterialIds.push_back( 0 );
    EXPECT_NE( refusal( duplicate ).find( "refused" ), std::string::npos ) << refusal( duplicate );

    EditMeshSer shortGroups = good;
    shortGroups.PolyGroups.pop_back();
    EXPECT_NE( refusal( shortGroups ).find( "polygroups" ), std::string::npos ) << refusal( shortGroups );
}

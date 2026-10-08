// DST-06: a fractured entity's pieces are static-mesh draws — which nodes draw, where (body pose / rest pose /
// preview Explode offset) and with which material per submesh (Engine/Destruction/FracturePieces.hpp), and the
// census that no second mesh renderer was added for them.

#include <Engine/Destruction/FractureEdit.hpp>
#include <Engine/Destruction/FracturePieces.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Destruction;

    FractureNode Leaf( int32_t parent, const glm::dvec3& centre )
    {
        FractureNode node;
        node.Parent         = parent;
        node.Level          = 1;
        node.CenterOfMass   = centre;
        node.Mesh.Positions = { 0, 0, 0, 10, 0, 0, 0, 10, 0 };
        node.Mesh.Triangles = { 0, 1, 2 };
        return node;
    }

    // Root (whole crate, no geometry of its own once cut) and three level-1 pieces along X, cm.
    std::vector<FractureNode> ThreePieces()
    {
        FractureNode root;
        root.CenterOfMass = glm::dvec3( 0.0 );
        return { root, Leaf( 0, { -50, 0, 0 } ), Leaf( 0, { 0, 0, 0 } ), Leaf( 0, { 50, 0, 0 } ) };
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadText( const std::string& path )
    {
        std::ifstream      in( RepoRoot() + path );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// N pieces -> N instances; at rest every piece is at the entity's world transform, simulated each takes its
// body's transform, and a piece whose body is gone is not drawn. Red when the root (no geometry) draws, a piece
// is dropped, or a transform is not the one its source gives.
TEST( FracturePieces, EachPieceIsAnInstanceAtItsBodysPoseOrAtRest )
{
    const auto      nodes = ThreePieces();
    const glm::mat4 world = glm::translate( glm::mat4( 1.0f ), glm::vec3( 100.0f, 200.0f, 300.0f ) );

    const auto rest = PieceInstances( nodes, world, {}, {} );
    ASSERT_EQ( rest.size(), 3u );
    for ( size_t i = 0; i < rest.size(); ++i )
    {
        EXPECT_EQ( rest[i].Node, static_cast<int32_t>( i + 1 ) );
        EXPECT_EQ( rest[i].Transform, world );
    }

    std::vector<std::optional<glm::mat4>> bodies( nodes.size() );
    bodies[1] = glm::translate( glm::mat4( 1.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) );
    bodies[2] = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 2.0f, 0.0f ) );
    // bodies[3] empty: removed on sleep
    const auto simulated = PieceInstances( nodes, world, bodies, {} );
    ASSERT_EQ( simulated.size(), 2u );
    EXPECT_EQ( simulated[0].Node, 1 );
    EXPECT_EQ( simulated[0].Transform, *bodies[1] );
    EXPECT_EQ( simulated[1].Node, 2 );
    EXPECT_EQ( simulated[1].Transform, *bodies[2] );
}

// The bake's interior ID draws the fracture's interior material; every other ID the source slot of that ID
// (clamped to the last slot). Red when the interior submesh takes a source slot or an exterior one the interior.
TEST( FracturePieces, InteriorSlotDrawsTheInteriorMaterialAndTheRestTheSourceMaterials )
{
    const std::vector<int> ids{ 0, 1, 3 }; // ascending, as ToRenderMesh emits; 3 = interior
    const auto             materials = PieceSubmeshMaterials( ids, /*interior*/ 3, /*source slots*/ 2 );
    ASSERT_EQ( materials.size(), 3u );
    EXPECT_FALSE( materials[0].Interior );
    EXPECT_EQ( materials[0].SourceSlot, 0u );
    EXPECT_FALSE( materials[1].Interior );
    EXPECT_EQ( materials[1].SourceSlot, 1u );
    EXPECT_TRUE( materials[2].Interior );

    const auto clamped = PieceSubmeshMaterials( std::vector<int>{ 2, 5 }, 5, 1 );
    EXPECT_FALSE( clamped[0].Interior );
    EXPECT_EQ( clamped[0].SourceSlot, 0u );
    EXPECT_TRUE( clamped[1].Interior );
}

// A missing interior material is an error that names the fracture (and the GUID it names), never a silent pick.
// Red when a null or unknown GUID passes, or the message does not name the fracture.
TEST( FracturePieces, AMissingInteriorMaterialIsAnErrorThatNamesTheFracture )
{
    FractureData fracture;
    fracture.InteriorMaterialId = 2;
    const auto none = ChooseInteriorMaterial( fracture, "Crate.dfrac", []( const auto& ) { return true; } );
    EXPECT_TRUE( none.Material.IsNull() );
    EXPECT_NE( none.Error.find( "Crate.dfrac" ), std::string::npos ) << none.Error;

    fracture.InteriorMaterial.Hi = 0x1234;
    fracture.InteriorMaterial.Lo = 0x5678;
    const auto unknown = ChooseInteriorMaterial( fracture, "Crate.dfrac", []( const auto& ) { return false; } );
    EXPECT_TRUE( unknown.Material.IsNull() );
    EXPECT_NE( unknown.Error.find( "Crate.dfrac" ), std::string::npos ) << unknown.Error;
    EXPECT_NE( unknown.Error.find( "not a material" ), std::string::npos ) << unknown.Error;

    const auto known = ChooseInteriorMaterial( fracture, "Crate.dfrac", []( const auto& ) { return true; } );
    EXPECT_TRUE( known.Error.empty() );
    EXPECT_EQ( known.Material, fracture.InteriorMaterial );
}

// The Explode offset moves rest-pose pieces only when the preview passes offsets; a simulated fracture ignores
// them; and the preview component is editor state (not reflected, not registered for serialization).
TEST( FracturePieces, TheExplodeOffsetIsPreviewStateOnly )
{
    const auto           nodes = ThreePieces();
    FractureViewSettings view;
    view.ExplodeAmount  = 1.0f;
    const auto offsets  = ExplodedOffsets( nodes, view );
    const auto exploded = PieceInstances( nodes, glm::mat4( 1.0f ), {}, offsets );
    ASSERT_EQ( exploded.size(), 3u );
    EXPECT_FLOAT_EQ( exploded[0].Transform[3].x, -50.0f );
    EXPECT_FLOAT_EQ( exploded[2].Transform[3].x, 50.0f );

    const auto assembled = PieceInstances( nodes, glm::mat4( 1.0f ), {}, {} );
    EXPECT_FLOAT_EQ( assembled[0].Transform[3].x, 0.0f );

    std::vector<std::optional<glm::mat4>> bodies( nodes.size(), glm::mat4( 1.0f ) );
    for ( const auto& piece : PieceInstances( nodes, glm::mat4( 1.0f ), bodies, offsets ) )
        EXPECT_EQ( piece.Transform, glm::mat4( 1.0f ) ) << "a simulated piece took the preview offset";

    const std::string preview = ReadText( "Desert/Desert/Source/Engine/ECS/FracturePreviewComponent.hpp" );
    ASSERT_FALSE( preview.empty() );
    EXPECT_EQ( preview.find( "REFLECT" ), std::string::npos );
    EXPECT_EQ( preview.find( "PROPERTY" ), std::string::npos );
    const std::string registry = ReadText( "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp" );
    ASSERT_FALSE( registry.empty() );
    EXPECT_EQ( registry.find( "FracturePreview" ), std::string::npos );
    const std::string destructible = ReadText( "Desert/Desert/Source/Engine/ECS/DestructibleComponent.hpp" );
    EXPECT_EQ( destructible.find( "Explode" ), std::string::npos );
}

// No second mesh renderer: the pieces are DrawStaticMeshCommands recorded by MeshECSSystem, and the mesh
// renderer's sources know nothing of fractures. Red when a piece renderer / submit path appears.
TEST( FracturePieces, PiecesAreStaticMeshDrawsNotASecondRenderer )
{
    const std::string draw = ReadText( "Desert/Desert/Source/Engine/ECS/System/FracturePieceDraw.cpp" );
    ASSERT_FALSE( draw.empty() );
    EXPECT_NE( draw.find( "Emplace<Graphic::Render::DrawStaticMeshCommand>" ), std::string::npos );
    EXPECT_EQ( draw.find( "RenderSystem" ), std::string::npos );
    EXPECT_EQ( draw.find( "renderer." ), std::string::npos );

    const std::string meshSystem = ReadText( "Desert/Desert/Source/Engine/ECS/System/MeshECSSystem.hpp" );
    EXPECT_NE( meshSystem.find( "m_Pieces.Record(" ), std::string::npos );
    EXPECT_NE( meshSystem.find( "drawnAsPieces.contains( entity )" ), std::string::npos );

    const std::filesystem::path renderers = RepoRoot() + "Desert/Desert/Source/Engine/Graphic";
    ASSERT_TRUE( std::filesystem::exists( renderers ) );
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( renderers ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string name = entry.path().filename().string();
        EXPECT_EQ( name.find( "Fracture" ), std::string::npos ) << entry.path();
        EXPECT_EQ( name.find( "Destruct" ), std::string::npos ) << entry.path();
    }
}

// Mutation: drop Scene::PrepareComponentPools' FracturePreviewComponent line (the parallel collector then creates
// the pool and races EnTT's pool vector), ask the FractureService from another parallel collector, make a serial
// system that asks it parallel, emit pieces without their own motion part, or free piece vertex buffers outside
// the allocator's ring -> red.
TEST( FracturePieces, ThePieceDrawIsRaceFreeAndEachPieceHasItsOwnMotionKey )
{
    const std::string scene = ReadText( "Desert/Desert/Source/Engine/Core/Scene.cpp" );
    EXPECT_NE( scene.find( "r.prepare<ECS::FracturePreviewComponent>();" ), std::string::npos );
    EXPECT_NE( scene.find( "r.prepare<ECS::DestructibleComponent>();" ), std::string::npos );

    const std::string systemDir = "Desert/Desert/Source/Engine/ECS/System/";
    size_t            askers    = 0;
    for ( const auto& entry : std::filesystem::directory_iterator( RepoRoot() + systemDir ) )
    {
        const std::string name = entry.path().filename().string();
        if ( entry.path().extension() != ".hpp" || name == "MeshECSSystem.hpp" )
            continue;
        const std::string text = ReadText( systemDir + name );
        if ( text.find( "GetFractureService" ) == std::string::npos )
            continue;
        ++askers;
        EXPECT_EQ( text.find( "CanRunParallel" ), std::string::npos )
             << name << " asks the FractureService and may run in a parallel group with MeshECSSystem";
    }
    EXPECT_GE( askers, 1u ) << "PhysicsECSSystem no longer found asking the FractureService: re-check the census";

    const std::string draw = ReadText( systemDir + "FracturePieceDraw.cpp" );
    EXPECT_NE( draw.find( "/*motionPart*/ static_cast<uint32_t>( instance.Node ) + 1u" ), std::string::npos )
         << "every piece draws with its own MotionHistory key";

    const std::string vertex = ReadText( "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanVertexBuffer.cpp" );
    EXPECT_NE( vertex.find( "RT_DestroyBuffer" ), std::string::npos );
    EXPECT_EQ( vertex.find( "vmaDestroyBuffer" ), std::string::npos ) << "a buffer freed under a frame in flight";
}

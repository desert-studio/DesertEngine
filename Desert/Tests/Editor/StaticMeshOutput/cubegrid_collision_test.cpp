// CG-BAKE1: Cube Grid's Accept gave a piece a BOX around it, so a room built of blocks was solid inside and could
// not be walked into. Pinned here on the editor's own inputs: a baked room with a doorway, put on an entity the
// way CubeGridTool::RegenMesh does (FromRenderMesh -> SetEditableMeshFromEditMesh, upload aside), the collision Accept gives it
// (GiveBlockoutCollision), and the body Play builds from that collider (PhysicsECSSystem::GatherColliderMesh ->
// PhysicsWorld::CreateBody). A ray through the doorway must reach the far wall's inner face.

#include <Editor/Panels/ViewportPanel/Tools/BlockoutCollision.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/System/ColliderMesh.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/VoxelBlockout.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <optional>

using namespace Desert;

namespace
{
    constexpr float kBlockCm = 100.0f;

    // A one-block-high room, three by three blocks in plan with its middle empty and a doorway at the middle of
    // its z = 0 side: cell (x, 0, z) spans [x, x + 1) * kBlockCm, so the room's inside is x, z in [100, 200) cm
    // and the doorway is x in [100, 200) cm at z in [0, 100) cm.
    Geometry::VoxelBlockout::Volume RoomWithADoorway()
    {
        Geometry::VoxelBlockout::Volume volume;
        volume.m_Unit = kBlockCm;
        for ( int x = 0; x < 3; ++x )
            for ( int z = 0; z < 3; ++z )
            {
                const bool middle  = x == 1 && z == 1;
                const bool doorway = x == 1 && z == 0;
                if ( !middle && !doorway )
                    volume.m_Cells[Geometry::VoxelBlockout::Pack( { x, 0, z } )] = {};
            }
        return volume;
    }

    // The piece as CubeGridTool::RegenMesh leaves it: a StaticMesh whose edited mesh is the bake and whose
    // runtime mesh is that mesh's render form - ECS::SetEditableMesh's two members, built the way it builds
    // them (Bridge::FromEditMesh, ToRenderMesh) minus its GPU upload: this suite has no device, and the body
    // Play makes reads the runtime mesh's CPU vertices (ColliderMeshSource::RuntimeMesh), never its buffers.
    Common::BoolResultStr PutOnEntity( const Geometry::VoxelBlockout::Volume& volume, const ECS::Entity& piece )
    {
        auto edit = Geometry::FromRenderMesh( volume.Bake() );
        if ( !edit.IsSuccess() )
            return Common::MakeError<bool>( edit.GetError() );
        auto editable = Geometry::Bridge::FromEditMesh( std::move( edit.ExtractValue().Mesh ) );
        if ( !editable.IsSuccess() )
            return Common::MakeError<bool>( editable.GetError() );
        auto render = Geometry::ToRenderMesh( *editable.GetValue() );
        if ( !render.IsSuccess() )
            return Common::MakeError<bool>( render.GetError() );
        const Geometry::RenderMeshData& data = render.GetValue();

        auto& mesh        = piece.AddComponent<ECS::StaticMeshComponent>();
        mesh.EditableMesh = editable.ExtractValue();
        mesh.RuntimeMesh  = std::make_shared<DynamicMesh>( data.Vertices, data.Indices, data.Submeshes );
        (void)ECS::CoverMaterialIds( mesh.MaterialSlots, data.SubmeshMaterialIds );
        return Common::MakeSuccess( true );
    }

    // What Play builds from the entity's collider and body (PhysicsECSSystem's body creation), at the origin.
    struct PlayedBody
    {
        Physics::PhysicsWorld                  World;
        std::optional<ECS::ColliderMesh>       Mesh;
        Common::ResultStr<Physics::BodyHandle> Body = Common::MakeError<Physics::BodyHandle>( "not built" );

        PlayedBody( entt::registry& registry, entt::entity entity )
        {
            EXPECT_TRUE( World.Init( 981.0f ) );
            const auto&       collider = registry.get<ECS::ColliderComponent>( entity ).Data;
            Physics::BodyDesc desc;
            desc.Shape       = collider.Shape;
            desc.HalfExtents = collider.HalfExtents;
            desc.Radius      = collider.Radius;
            desc.HalfHeight  = collider.HalfHeight;
            desc.Axis        = collider.Axis;
            desc.Center      = collider.Center;
            desc.Type        = registry.get<ECS::RigidBodyComponent>( entity ).Data.Type;
            if ( desc.Shape == Physics::ShapeType::Mesh || desc.Shape == Physics::ShapeType::ConvexHull )
            {
                auto gathered = ECS::PhysicsECSSystem::GatherColliderMesh( registry, entity, glm::vec3( 1.0f ) );
                if ( !gathered.IsSuccess() )
                {
                    Body = Common::MakeError<Physics::BodyHandle>( gathered.GetError() );
                    return;
                }
                Mesh = gathered.GetValue();
                if ( !Mesh )
                {
                    Body = Common::MakeError<Physics::BodyHandle>( "the collider mesh is still loading" );
                    return;
                }
                desc.MeshPoints  = Mesh->Points;
                desc.MeshIndices = Mesh->Indices;
            }
            Body = World.CreateBody( desc );
        }
        ~PlayedBody()
        {
            World.Shutdown();
        }
        PlayedBody( const PlayedBody& )            = delete;
        PlayedBody& operator=( const PlayedBody& ) = delete;
    };
} // namespace

TEST( CubeGridCollision, AcceptedRoomIsWalkedIntoThroughItsDoorway )
{
    entt::registry    registry;
    const ECS::Entity piece( registry.create(), registry );
    const auto        put = PutOnEntity( RoomWithADoorway(), piece );
    ASSERT_TRUE( put.IsSuccess() ) << put.GetError();

    const auto given = Editor::Tools::GiveBlockoutCollision( piece );
    ASSERT_TRUE( given.IsSuccess() ) << given.GetError();
    EXPECT_EQ( piece.GetComponent<ECS::ColliderComponent>().Data.Shape, Physics::ShapeType::Mesh );
    EXPECT_EQ( piece.GetComponent<ECS::RigidBodyComponent>().Data.Type, Physics::BodyType::Static );

    PlayedBody played( registry, piece.GetHandle() );
    ASSERT_TRUE( played.Body.IsSuccess() ) << played.Body.GetError();

    // Through the doorway (x = 150 cm, half a block up): nothing until the far wall's inner face at z = 200 cm.
    // A bounding box stops the ray at the room's outside, z = 0.
    const auto through = played.World.CastRay( { 150.0f, 50.0f, -500.0f }, { 0.0f, 0.0f, 1.0f }, 5000.0f );
    ASSERT_TRUE( through.has_value() ) << "the far wall has no collision";
    // clang-tidy 18 does not see gtest's ASSERT as the check it is.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( through->Point.z, 2.0f * kBlockCm, 0.5f ) << "the ray stopped before the room's far wall";
    EXPECT_NEAR( through->Distance, 700.0f, 0.5f );
    // NOLINTEND(bugprone-unchecked-optional-access)

    // Beside the doorway the wall is solid from its outside face, z = 0.
    const auto wall = played.World.CastRay( { 50.0f, 50.0f, -500.0f }, { 0.0f, 0.0f, 1.0f }, 5000.0f );
    ASSERT_TRUE( wall.has_value() ) << "the front wall has no collision";
    EXPECT_NEAR( wall->Point.z, 0.0f, 0.5f ); // NOLINT(bugprone-unchecked-optional-access)

    // Down into the open-topped room: no roof and no floor, so nothing at all.
    EXPECT_FALSE( played.World.CastRay( { 150.0f, 500.0f, 150.0f }, { 0.0f, -1.0f, 0.0f }, 5000.0f ).has_value() )
         << "the room's inside collides: it is filled, not hollow";
}

TEST( CubeGridCollision, APieceWithNothingDrawnIsRefusedAndKeepsNoCollider )
{
    entt::registry    registry;
    const ECS::Entity bare( registry.create(), registry );
    const auto        noMesh = Editor::Tools::GiveBlockoutCollision( bare );
    ASSERT_FALSE( noMesh.IsSuccess() );
    EXPECT_NE( noMesh.GetError().find( "StaticMesh" ), std::string::npos ) << noMesh.GetError();

    bare.AddComponent<ECS::StaticMeshComponent>();
    const auto empty = Editor::Tools::GiveBlockoutCollision( bare );
    ASSERT_FALSE( empty.IsSuccess() );
    EXPECT_NE( empty.GetError().find( "no mesh" ), std::string::npos ) << empty.GetError();
    EXPECT_FALSE( bare.HasComponent<ECS::ColliderComponent>() );
    EXPECT_FALSE( bare.HasComponent<ECS::RigidBodyComponent>() );
}

TEST( CubeGridCollision, AReacceptedPieceWithABoxColliderGetsItsTriangles )
{
    entt::registry    registry;
    const ECS::Entity piece( registry.create(), registry );
    const auto        put = PutOnEntity( RoomWithADoorway(), piece );
    ASSERT_TRUE( put.IsSuccess() ) << put.GetError();
    // A reopened piece accepted before CG-BAKE1 carries the old bounding box, here on a body someone made dynamic.
    piece.AddComponent<ECS::ColliderComponent>().Data.Shape = Physics::ShapeType::Box;
    piece.AddComponent<ECS::RigidBodyComponent>().Data.Type = Physics::BodyType::Dynamic;

    const auto given = Editor::Tools::GiveBlockoutCollision( piece );
    ASSERT_TRUE( given.IsSuccess() ) << given.GetError();
    EXPECT_EQ( piece.GetComponent<ECS::ColliderComponent>().Data.Shape, Physics::ShapeType::Mesh );
    EXPECT_EQ( piece.GetComponent<ECS::RigidBodyComponent>().Data.Type, Physics::BodyType::Static );
}

// RAG1b part 2: the ragdoll COMPONENT and its system on an in-memory registry and a real (device-free) Jolt world
// — the reflected block round-trips, the component makes a ragdoll and the entity going removes it, a skeleton
// mismatch is refused by name, a Kinematic -> Simulated switch in Play keeps the pose, and the system sits
// around the physics step after the animator.
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/RagdollLifetime.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Runtime/Services/Physics/PhysicsAssetService.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Physics;
using Desert::Animation::BoneInfo;
using Desert::Animation::Skeleton;
using Desert::ECS::RagdollLifetime;
using Guid = Common::Content::AssetGuid;

namespace
{
    constexpr float kStep   = 1.0f / 60.0f;
    constexpr float kLinkCm = 30.0f;
    const Guid      kSkeleton{ 0xBEEFull, 0x0002ull };
    const Guid      kOtherSkeleton{ 0xBEEFull, 0x0003ull };

    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::mat4& local )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = local;
        return b;
    }

    // a(0) -> b(1) -> c(2), each link kLinkCm along +Z; the root stands 100 cm up so a simulated chain falls.
    Skeleton MakeChain()
    {
        const glm::mat4       link = glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, kLinkCm } );
        std::vector<BoneInfo> bones;
        bones.push_back(
             MakeBone( "a", std::nullopt, glm::translate( glm::mat4( 1.0f ), { 0.0f, 100.0f, 0.0f } ) ) );
        bones.push_back( MakeBone( "b", 0u, link ) );
        bones.push_back( MakeBone( "c", 1u, link ) );
        return Skeleton( std::move( bones ) );
    }

    std::shared_ptr<const PhysicsAssetData> MakeChainAsset()
    {
        auto asset      = std::make_shared<PhysicsAssetData>();
        asset->Guid     = { 0xBEEFull, 0x0001ull };
        asset->Skeleton = kSkeleton;
        for ( const char* bone : { "a", "b", "c" } )
        {
            PhysicsAssetBody body;
            body.Bone   = bone;
            body.Shape  = PhysicsBodyShape::Capsule;
            body.Radius = 5.0f;
            body.Length = 20.0f;
            body.Center = { 0.0f, 0.0f, 0.5f * kLinkCm };
            body.MassKg = 5.0f;
            asset->Bodies.push_back( body );
        }
        for ( const auto& [parent, child] : { std::pair{ "a", "b" }, std::pair{ "b", "c" } } )
        {
            PhysicsAssetConstraint joint;
            joint.ParentBone         = parent;
            joint.ChildBone          = child;
            joint.Swing1LimitDegrees = 30.0f;
            joint.Swing2LimitDegrees = 30.0f;
            joint.TwistLimitDegrees  = 20.0f;
            asset->Constraints.push_back( joint );
        }
        return asset;
    }

    // What PhysicsECSSystem asks PhysicsAssetService, with the asset in memory: the same skeleton check.
    RagdollLifetime::PhysicsAssetLookup AssetLookup( std::shared_ptr<const PhysicsAssetData> asset )
    {
        return [asset]( const Desert::Assets::AssetHandle&, const Guid& meshSkeleton,
                        std::string_view name ) -> Common::ResultStr<std::shared_ptr<const PhysicsAssetData>>
        {
            auto match = Desert::Runtime::PhysicsAssetService::CheckSkeleton( *asset, meshSkeleton, name );
            if ( !match.IsSuccess() )
                return Common::MakeError<std::shared_ptr<const PhysicsAssetData>>( match.GetError() );
            return Common::MakeSuccess( asset );
        };
    }

    RagdollLifetime::MeshSkeletonLookup SkeletonLookup( Guid skeleton )
    {
        return [skeleton]( const entt::registry&, entt::entity ) { return Common::MakeSuccess( skeleton ); };
    }

    struct Scene
    {
        Skeleton       Rig = MakeChain();
        entt::registry Registry;
        entt::entity   Entity = entt::null;
        PhysicsWorld   World;

        explicit Scene( RagdollMotion mode )
        {
            EXPECT_TRUE( World.Init( 981.0f ) );
            Entity = Registry.create();
            Registry.emplace<Desert::ECS::TransformComponent>( Entity );
            auto animator = std::make_unique<Desert::Animation::Animator>( Rig );
            animator->ApplyLocalPose(); // the rendered pose is the bind pose
            Registry.emplace<Desert::ECS::AnimationComponent>( Entity, std::move( animator ) );
            auto& ragdoll             = Registry.emplace<Desert::ECS::RagdollComponent>( Entity );
            ragdoll.Data.PhysicsAsset = Desert::Assets::AssetHandle( 0x1234ull );
            ragdoll.Data.Mode         = mode;
        }

        Desert::Animation::Animator& Animator()
        {
            return *Registry.get<Desert::ECS::AnimationComponent>( Entity ).Animator;
        }

        // One frame of PhysicsECSSystem's order: drive, step, write back.
        void Frame( RagdollLifetime& lifetime, Guid meshSkeleton = kSkeleton )
        {
            lifetime.DriveBeforeStep( Registry, AssetLookup( MakeChainAsset() ), SkeletonLookup( meshSkeleton ) );
            World.Step( kStep );
            lifetime.WriteBackAfterStep( Registry );
        }
    };

    std::string ReadRepoFile( const std::string& relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream in( prefix + relative );
            if ( in )
            {
                std::ostringstream text;
                text << in.rdbuf();
                return text.str();
            }
            prefix += "../";
        }
        ADD_FAILURE() << relative << " is not readable from the test's working directory";
        return {};
    }
} // namespace

TEST( RagdollLifetime, ComponentRoundTripsThroughTheSceneText )
{
    const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( "RagdollData" );
    ASSERT_NE( type, nullptr ) << "RagdollData is not reflected, so no scene can carry a ragdoll";

    Desert::ECS::RagdollData written;
    written.PhysicsAsset = Desert::Assets::AssetHandle( 0xF0E1D2C3B4A59687ull );
    written.Mode         = RagdollMotion::Simulated;
    const auto object    = Desert::Reflection::SerializeReflected( *type, &written, nullptr );
    const auto text      = Common::Json::Write( object );
    const auto parsed    = Common::Json::Read<Common::Json::Object>( text );
    ASSERT_TRUE( parsed.IsSuccess() ) << text;

    Desert::ECS::RagdollData read; // defaults: null asset, Kinematic — so "unchanged" cannot pass as "read"
    Common::Json::Issues     issues;
    Desert::Reflection::DeserializeReflected(
         *type, &read, Common::Json::Root( Common::Json::Value( parsed.GetValue() ) ), issues, nullptr );
    for ( const auto& issue : issues )
        ADD_FAILURE() << Common::Json::Describe( issue );
    EXPECT_EQ( static_cast<uint64_t>( read.PhysicsAsset ), static_cast<uint64_t>( written.PhysicsAsset ) ) << text;
    EXPECT_EQ( read.Mode, RagdollMotion::Simulated ) << text;
}

TEST( RagdollLifetime, ComponentCreatesARagdollAndTheEntityGoingRemovesIt )
{
    Scene           scene( RagdollMotion::Kinematic );
    RagdollLifetime lifetime( scene.World );
    lifetime.Attach( scene.Registry );
    scene.Frame( lifetime );
    EXPECT_EQ( scene.World.GetRagdollCount(), 1u );
    EXPECT_NE( scene.Registry.get<Desert::ECS::RagdollComponent>( scene.Entity ).RuntimeRagdoll, kInvalidRagdoll );

    scene.Registry.destroy( scene.Entity );
    EXPECT_EQ( scene.World.GetRagdollCount(), 0u ) << "the entity went and its bodies stayed in the world";
    lifetime.Detach();
}

TEST( RagdollLifetime, SkeletonMismatchIsRefusedByNameAndNoRagdollIsCreated )
{
    const auto message =
         Desert::Runtime::PhysicsAssetService::CheckSkeleton( *MakeChainAsset(), kOtherSkeleton, "Mannequin" );
    ASSERT_FALSE( message.IsSuccess() ) << "an asset authored on another skeleton was accepted";
    for ( const std::string& named :
          { Common::Content::AssetGuidToText( kSkeleton ), Common::Content::AssetGuidToText( kOtherSkeleton ),
            std::string( "Mannequin" ) } )
        EXPECT_NE( message.GetError().find( named ), std::string::npos )
             << message.GetError() << " -- lacks " << named;
    EXPECT_TRUE(
         Desert::Runtime::PhysicsAssetService::CheckSkeleton( *MakeChainAsset(), kSkeleton, "x" ).IsSuccess() );

    Scene           scene( RagdollMotion::Simulated );
    RagdollLifetime lifetime( scene.World );
    lifetime.Attach( scene.Registry );
    for ( int frame = 0; frame < 3; ++frame )
        scene.Frame( lifetime, kOtherSkeleton );
    EXPECT_EQ( scene.World.GetRagdollCount(), 0u );
    EXPECT_EQ( scene.Registry.get<Desert::ECS::RagdollComponent>( scene.Entity ).RuntimeRagdoll, kInvalidRagdoll );
    lifetime.Detach();
}

TEST( RagdollLifetime, ModeSwitchInPlayKeepsThePoseContinuous )
{
    Scene           scene( RagdollMotion::Kinematic );
    RagdollLifetime lifetime( scene.World );
    lifetime.Attach( scene.Registry );
    for ( int frame = 0; frame < 10; ++frame )
        scene.Frame( lifetime );
    const Desert::Animation::LocalPose before = scene.Animator().GetLocalPose();

    // Lua's `entity.Ragdoll.Mode = Simulated` is this write.
    scene.Registry.get<Desert::ECS::RagdollComponent>( scene.Entity ).Data.Mode = RagdollMotion::Simulated;
    scene.Frame( lifetime );
    const RagdollHandle handle = scene.Registry.get<Desert::ECS::RagdollComponent>( scene.Entity ).RuntimeRagdoll;
    ASSERT_EQ( scene.World.GetRagdollMotion( handle ), RagdollMotion::Simulated );
    const Desert::Animation::LocalPose& after = scene.Animator().GetLocalPose();
    for ( uint32_t bone = 0; bone < 3; ++bone )
    {
        // One 1/60 s step from rest under 981 cm/s^2 moves a body ~0.3 cm; a reset to the bind or a jump is more.
        EXPECT_LT( glm::length( after[bone].Translation - before[bone].Translation ), 1.0f ) << "bone " << bone;
        EXPECT_GT( std::abs( glm::dot( after[bone].Rotation, before[bone].Rotation ) ), 0.999f )
             << "bone " << bone;
    }

    // Not vacuous: the simulation, not the animation, now moves the bones.
    for ( int frame = 0; frame < 30; ++frame )
        scene.Frame( lifetime );
    EXPECT_GT( glm::length( scene.Animator().GetLocalPose()[0].Translation - before[0].Translation ), 5.0f )
         << "the simulated root did not fall, so the write-back never reached the animator";
    lifetime.Detach();
}

TEST( RagdollLifetime, SystemRunsAroundThePhysicsStepAfterTheAnimator )
{
    const std::string physics = ReadRepoFile( "Desert/Desert/Source/Engine/ECS/System/PhysicsECSSystem.hpp" );
    const size_t      drive   = physics.find( "m_Ragdolls->DriveBeforeStep(" );
    const size_t      step    = physics.find( "m_World->Step(" );
    const size_t      back    = physics.find( "m_Ragdolls->WriteBackAfterStep(" );
    ASSERT_NE( drive, std::string::npos );
    ASSERT_NE( step, std::string::npos );
    ASSERT_NE( back, std::string::npos );
    EXPECT_LT( drive, step ) << "kinematic targets must be set before the step that reaches them";
    EXPECT_LT( step, back ) << "the simulated pose must be read after the step";

    // The animator's pose this frame is what the ragdoll is driven to and written over: Animation before Physics.
    for ( const char* host :
          { "Runtime/Source/RuntimeLayer.cpp", "Editor/Source/Editor/LevelEditor/SceneWorkspace.cpp" } )
    {
        const std::string text      = ReadRepoFile( host );
        const size_t      animation = text.find( "AnimationECSSystem>(" );
        const size_t      simulate  = text.find( "PhysicsECSSystem>(" );
        ASSERT_NE( animation, std::string::npos ) << host;
        ASSERT_NE( simulate, std::string::npos ) << host;
        EXPECT_LT( animation, simulate ) << host << " adds Physics before Animation";
    }
}

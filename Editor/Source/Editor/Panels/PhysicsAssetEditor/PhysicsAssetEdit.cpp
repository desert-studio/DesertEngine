#include "PhysicsAssetEdit.hpp"

#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/PhysicsAsset.hpp>
#include <Engine/Physics/RagdollPose.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        constexpr float kPreviewGravityCmPerS2 = 981.0f;

        bool LimitInRange( float degrees )
        {
            return degrees >= 0.0f && degrees <= 180.0f;
        }
    } // namespace

    PhysicsAssetEditCommand::PhysicsAssetEditCommand( std::shared_ptr<Physics::PhysicsAssetData> target,
                                                      Physics::PhysicsAssetData                  before,
                                                      Physics::PhysicsAssetData after, std::string label )
         : m_Target( std::move( target ) ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
           m_Label( std::move( label ) )
    {
    }

    bool PhysicsAssetEditCommand::Undo()
    {
        *m_Target = m_Before;
        return true;
    }

    bool PhysicsAssetEditCommand::Redo()
    {
        *m_Target = m_After;
        return true;
    }

    bool CommitPhysicsAssetEdit( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                 Physics::PhysicsAssetData after, const std::string& label )
    {
        if ( !target || *target == after )
            return false;
        Physics::PhysicsAssetData before = *target;
        *target                          = after;
        CommandHistory::Get().PushCommand(
             std::make_unique<PhysicsAssetEditCommand>( target, std::move( before ), std::move( after ), label ) );
        return true;
    }

    Common::BoolResultStr CommitJointLimits( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                             const std::string& childBone, float swing1Degrees,
                                             float swing2Degrees, float twistDegrees )
    {
        if ( !target )
            return Common::MakeError<bool>( "no physics asset is open" );
        if ( !LimitInRange( swing1Degrees ) || !LimitInRange( swing2Degrees ) || !LimitInRange( twistDegrees ) )
            return Common::MakeFormattedError<bool>( "joint '{}': limits must lie in [0, 180] degrees",
                                                     childBone );
        Physics::PhysicsAssetData after = *target;
        const auto                joint = std::find_if( after.Constraints.begin(), after.Constraints.end(),
                                                        [&]( const auto& c ) { return c.ChildBone == childBone; } );
        if ( joint == after.Constraints.end() )
            return Common::MakeFormattedError<bool>( "no joint has '{}' as its child bone", childBone );
        joint->Swing1LimitDegrees = swing1Degrees;
        joint->Swing2LimitDegrees = swing2Degrees;
        joint->TwistLimitDegrees  = twistDegrees;
        CommitPhysicsAssetEdit( target, std::move( after ), "Joint Limits" );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr CommitBody( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                      const Physics::PhysicsAssetBody&                  body )
    {
        if ( !target )
            return Common::MakeError<bool>( "no physics asset is open" );
        if ( body.Radius < 0.0f || body.Length < 0.0f || body.MassKg < 0.0f ||
             glm::any( glm::lessThan( body.BoxExtents, glm::vec3( 0.0f ) ) ) )
            return Common::MakeFormattedError<bool>( "body '{}': sizes and mass cannot be negative", body.Bone );
        Physics::PhysicsAssetData after = *target;
        const auto                found = std::find_if( after.Bodies.begin(), after.Bodies.end(),
                                                        [&]( const auto& b ) { return b.Bone == body.Bone; } );
        if ( found == after.Bodies.end() )
            return Common::MakeFormattedError<bool>( "bone '{}' has no body", body.Bone );
        *found = body;
        CommitPhysicsAssetEdit( target, std::move( after ), "Body" );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SavePhysicsAssetDocument( const Common::Filepath&          path,
                                                    const Physics::PhysicsAssetData& data )
    {
        return Assets::PhysicsAsset::Save( path, data );
    }

    PhysicsAssetPreviewSimulation::PhysicsAssetPreviewSimulation()  = default;
    PhysicsAssetPreviewSimulation::~PhysicsAssetPreviewSimulation() = default;

    Common::BoolResultStr PhysicsAssetPreviewSimulation::Start( const Physics::PhysicsAssetData& asset,
                                                                const Animation::Skeleton&       skeleton,
                                                                const Animation::LocalPose&      pose )
    {
        Stop();
        auto desc = Physics::BuildRagdollDesc( asset, skeleton );
        if ( !desc )
            return Common::MakeError<bool>( desc.GetError() );

        auto world = std::make_unique<Physics::PhysicsWorld>();
        if ( !world->Init( kPreviewGravityCmPerS2 ) )
            return Common::MakeError<bool>( "the preview physics world would not initialise" );

        Physics::BodyDesc floor;
        floor.Shape       = Physics::ShapeType::Box;
        floor.HalfExtents = { 2000.0f, 10.0f, 2000.0f };
        floor.Position    = { 0.0f, -10.0f, 0.0f }; // top face at y = 0, the preview's floor
        floor.Type        = Physics::BodyType::Static;
        if ( const auto made = world->CreateBody( floor ); !made )
            return Common::MakeError<bool>( made.GetError() );

        const glm::mat4 entityWorld( 1.0f );
        const auto      parts = Physics::RagdollPartsFromPose( desc.GetValue(), skeleton, pose, entityWorld );
        if ( !parts )
            return Common::MakeError<bool>( parts.GetError() );
        const auto ragdoll =
             world->CreateRagdoll( desc.GetValue(), glm::vec3( 0.0f ), glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ),
                                   Physics::RagdollMotion::Simulated );
        if ( !ragdoll )
            return Common::MakeError<bool>( ragdoll.GetError() );
        world->SetRagdollPose( ragdoll.GetValue(), parts.GetValue() );

        m_Desc    = desc.GetValue();
        m_Ragdoll = ragdoll.GetValue();
        m_World   = std::move( world );
        m_World->GetRagdollPose( m_Ragdoll, m_Parts );
        return Common::MakeSuccess( true );
    }

    void PhysicsAssetPreviewSimulation::Step( float dt )
    {
        if ( !m_World )
            return;
        m_World->Step( dt );
        m_World->GetRagdollPose( m_Ragdoll, m_Parts );
    }

    void PhysicsAssetPreviewSimulation::Stop()
    {
        m_World.reset(); // the world removes its ragdoll before its system dies
        m_Ragdoll = Physics::kInvalidRagdoll;
        m_Parts.clear();
    }

    Common::ResultStr<std::vector<Animation::BoneOverride>>
    PhysicsAssetPreviewSimulation::Overrides( const Animation::Skeleton&  skeleton,
                                              const Animation::LocalPose& animated )
    {
        if ( !m_World )
            return Common::MakeError<std::vector<Animation::BoneOverride>>( "the preview is not simulating" );
        return Physics::RagdollBoneOverrides( m_Desc, skeleton, animated, glm::mat4( 1.0f ), m_Parts );
    }
} // namespace Desert::Editor

#include <Engine/GameplayDebugger/GameplayDebuggerCategoryPhysics.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/PhysicsEvents.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>

#include <PhysicsCore/PhysicsWorld.hpp>

#include <format>
#include <string>

namespace Desert::Debug
{
    namespace
    {
        const glm::vec4 kPhysicsLabel( 0.65f, 0.65f, 0.65f, 1.0f );
        const glm::vec4 kPhysicsValue( 1.0f, 1.0f, 1.0f, 1.0f );
        const glm::vec4 kPhysicsAwake( 0.35f, 1.0f, 0.35f, 1.0f );
        const glm::vec4 kPhysicsAsleep( 0.45f, 0.6f, 1.0f, 1.0f );
        const glm::vec4 kPhysicsVelocity( 0.3f, 0.9f, 1.0f, 1.0f );
        const glm::vec4 kPhysicsHit( 1.0f, 0.35f, 0.25f, 1.0f );
        const glm::vec4 kPhysicsOverlap( 1.0f, 0.8f, 0.3f, 1.0f );
        // How many contacts the recent list keeps.
        constexpr size_t kPhysicsRecentContacts = 6;
        // The cross at a contact point and the length of its normal, cm.
        constexpr float kPhysicsContactHalfSize = 8.0f;
        constexpr float kPhysicsNormalLength    = 40.0f;

        void PhysicsRow( GameplayDebuggerCategoryData& out, std::string label, std::string value,
                  const glm::vec4& valueColor = kPhysicsValue )
        {
            out.AddTextLine( { { std::move( label ), kPhysicsLabel }, { std::move( value ), valueColor } } );
        }

        const char* PhysicsBodyTypeName( const Physics::BodyType type )
        {
            switch ( type )
            {
                case Physics::BodyType::Static:
                    return "Static";
                case Physics::BodyType::Dynamic:
                    return "Dynamic";
                case Physics::BodyType::Kinematic:
                    return "Kinematic";
            }
            return "?";
        }

        const char* PhysicsEventName( const ECS::PhysicsEventKind kind )
        {
            switch ( kind )
            {
                case ECS::PhysicsEventKind::Hit:
                    return "Hit";
                case ECS::PhysicsEventKind::BeginOverlap:
                    return "Begin overlap";
                case ECS::PhysicsEventKind::EndOverlap:
                    return "End overlap";
            }
            return "?";
        }

        /// What the panel calls @p other: its tag, "<untagged>", "<destroyed>" for an End whose entity is gone,
        /// "<world>" for a body no entity owns (a landscape tile, a destructible's piece).
        std::string PhysicsOtherName( const entt::registry& registry, const entt::entity other )
        {
            if ( other == entt::null )
                return "<world>";
            if ( !registry.valid( other ) )
                return "<destroyed>";
            const auto* tag = registry.try_get<ECS::TagComponent>( other );
            return tag != nullptr && !tag->Tag.empty() ? tag->Tag : std::string( "<untagged>" );
        }

        std::string PhysicsEventText( const entt::registry& registry, const ECS::PhysicsEvent& event )
        {
            if ( event.Kind == ECS::PhysicsEventKind::Hit )
                return std::format( "{} {} {:.1f} kg*cm/s", PhysicsEventName( event.Kind ),
                                    PhysicsOtherName( registry, event.Other ), event.Impulse );
            return std::format( "{} {}", PhysicsEventName( event.Kind ), PhysicsOtherName( registry, event.Other ) );
        }

        std::string PhysicsVec3Text( const glm::vec3& v, const char* units )
        {
            return std::format( "{:.1f} {:.1f} {:.1f} {} ({:.1f})", v.x, v.y, v.z, units, glm::length( v ) );
        }
    } // namespace

    void GameplayDebuggerCategoryPhysics::ReadQueue( const Core::Scene& scene, const entt::entity target )
    {
        if ( m_Watched != target )
        {
            m_Watched = target;
            m_Recent.clear();
        }
        const auto* queue = scene.GetRegistry().try_ctx<ECS::PhysicsEventQueue>();
        if ( queue == nullptr || queue->Publication == m_ReadPublication )
            return;
        m_ReadPublication = queue->Publication;
        for ( const ECS::PhysicsEvent& event : queue->Events )
        {
            if ( event.Self != target )
                continue;
            m_Recent.push_front( SeenContact{ PhysicsEventText( scene.GetRegistry(), event ) } );
            if ( m_Recent.size() > kPhysicsRecentContacts )
                m_Recent.pop_back();
        }
    }

    void GameplayDebuggerCategoryPhysics::CollectData( const Core::Scene& scene, const ECS::Entity* targetEntity,
                                                       GameplayDebuggerCategoryData& out )
    {
        const entt::registry& registry = scene.GetRegistry();
        if ( targetEntity == nullptr || !registry.valid( targetEntity->GetHandle() ) )
        {
            m_Watched.reset();
            m_Recent.clear();
            out.AddTextLine( { { "no target", kPhysicsLabel } } );
            return;
        }
        const entt::entity target = targetEntity->GetHandle();

        const auto* rigidBody = registry.try_get<ECS::RigidBodyComponent>( target );
        const auto* character = registry.try_get<ECS::CharacterControllerComponent>( target );
        if ( rigidBody == nullptr && character == nullptr )
        {
            m_Watched.reset();
            m_Recent.clear();
            out.AddTextLine( { { "no physics: neither a RigidBody nor a CharacterController", kPhysicsLabel } } );
            return;
        }

        // The world exists while Play runs (PhysicsECSSystem::GetPhysicsWorld); in Edit only the authored data
        // can be shown.
        const auto*                  system = scene.FindSystem<ECS::PhysicsECSSystem>();
        const Physics::PhysicsWorld* world  = system != nullptr ? system->GetPhysicsWorld() : nullptr;

        const glm::vec3     location = glm::vec3( targetEntity->GetWorldTransform()[3] );
        Physics::BodyHandle body     = Physics::kInvalidBody;
        if ( rigidBody != nullptr )
        {
            const ECS::RigidBodyData& data = rigidBody->Data;
            PhysicsRow( out, "Body     ",
                 data.Type == Physics::BodyType::Dynamic
                      ? std::format( "{} {:.2f} kg, profile {}", PhysicsBodyTypeName( data.Type ), data.Mass,
                                     data.CollisionProfile )
                      : std::format( "{}, profile {}", PhysicsBodyTypeName( data.Type ), data.CollisionProfile ) );
            body = rigidBody->RuntimeBody;
        }
        if ( character != nullptr )
        {
            const ECS::CharacterControllerData& data = character->Data;
            PhysicsRow( out, "Character",
                 std::format( " capsule r {:.1f} h {:.1f} cm, profile {}", data.Radius,
                              character->IsCrouched ? data.CrouchedHeight : data.Height, data.CollisionProfile ) );
            PhysicsRow( out, "Movement ",
                 std::format( "{}{}{}", character->OnGround ? "on ground" : "falling",
                              character->IsCrouched ? ", crouched" : "",
                              character->Swimming ? ", swimming" : "" ) );
            PhysicsRow( out, "Velocity ", PhysicsVec3Text( character->Velocity, "cm/s" ) );
            if ( glm::length( character->Velocity ) > 0.0f )
                out.AddShape( { .Type   = GameplayDebuggerShape::Kind::Arrow,
                                .A      = location,
                                .B      = location + character->Velocity,
                                .Radius = kPhysicsContactHalfSize * 2.0f,
                                .Color  = kPhysicsVelocity } );
            if ( world != nullptr && character->RuntimeCharacter != Physics::kInvalidCharacter )
                body = world->GetCharacterBody( character->RuntimeCharacter );
        }

        if ( world == nullptr || body == Physics::kInvalidBody )
        {
            out.AddTextLine( { { "not simulated: the body exists while Play runs", kPhysicsLabel } } );
            m_Watched.reset();
            m_Recent.clear();
            return;
        }

        // A character's velocity is the movement model's (above); its inner body is what the others touch.
        if ( rigidBody != nullptr && rigidBody->RuntimeBody == body )
        {
            const glm::vec3 linear = world->GetLinearVelocity( body );
            PhysicsRow( out, "Velocity ", PhysicsVec3Text( linear, "cm/s" ) );
            PhysicsRow( out, "Angular  ", PhysicsVec3Text( world->GetAngularVelocity( body ), "rad/s" ) );
            if ( glm::length( linear ) > 0.0f )
                out.AddShape( { .Type   = GameplayDebuggerShape::Kind::Arrow,
                                .A      = location,
                                .B      = location + linear,
                                .Radius = kPhysicsContactHalfSize * 2.0f,
                                .Color  = kPhysicsVelocity } );
            if ( rigidBody->Data.Type == Physics::BodyType::Static )
                PhysicsRow( out, "State    ", "static (never simulated)", kPhysicsAsleep );
            else
                PhysicsRow( out, "State    ", world->IsActive( body ) ? "awake" : "sleeping",
                     world->IsActive( body ) ? kPhysicsAwake : kPhysicsAsleep );
        }

        // The step's contacts: listed and drawn where they touch.
        size_t stepContacts = 0;
        if ( const auto* queue = registry.try_ctx<ECS::PhysicsEventQueue>() )
            for ( const ECS::PhysicsEvent& event : queue->Events )
            {
                if ( event.Self != target )
                    continue;
                ++stepContacts;
                if ( event.Kind == ECS::PhysicsEventKind::EndOverlap )
                    continue; // no point: the pair no longer touches
                const glm::vec4& color = event.Kind == ECS::PhysicsEventKind::Hit ? kPhysicsHit : kPhysicsOverlap;
                out.AddShape( { .Type        = GameplayDebuggerShape::Kind::Point,
                                .A           = event.Point,
                                .Radius      = kPhysicsContactHalfSize,
                                .Color       = color,
                                .Description = PhysicsOtherName( registry, event.Other ) } );
                out.AddShape( { .Type   = GameplayDebuggerShape::Kind::Arrow,
                                .A      = event.Point,
                                .B      = event.Point + event.Normal * kPhysicsNormalLength,
                                .Radius = kPhysicsContactHalfSize,
                                .Color  = color } );
            }
        PhysicsRow( out, "Contacts ", std::format( "{} this frame", stepContacts ) );

        ReadQueue( scene, target );
        if ( m_Recent.empty() )
            out.AddTextLine( { { "  none seen while shown", kPhysicsLabel } } );
        for ( const SeenContact& seen : m_Recent )
            out.AddTextLine( { { "  " + seen.Text, kPhysicsValue } } );
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS

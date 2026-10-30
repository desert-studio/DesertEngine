#include <Engine/ECS/System/ProjectileMovement.hpp>

#include <Engine/ECS/Entity.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>

namespace Desert::ECS::ProjectileMovement
{
    namespace
    {
        // UE PullBackHit: the sweep stops this far (cm) short of the contact, so the next sweep does not start
        // touching the surface it just struck.
        constexpr float kPullBackDistance = 0.1f;
        // UE HandleDeflection's DotTolerance: a velocity this little out of the surface slides along it.
        constexpr float kSlideDotTolerance = 0.01f;

        // UE GetSimulationTimeStep: a gravity-bent flight is cut into sub-steps of at most
        // kMaxSimulationTimeStep (halving a long remainder), and the last iteration takes whatever remains.
        float SimulationTimeStep( float remaining, int iterations )
        {
            if ( remaining > kMaxSimulationTimeStep && iterations < kMaxSimulationIterations )
                return std::min( kMaxSimulationTimeStep, remaining * 0.5f );
            return remaining;
        }

        // The world transform of the projectile's parent (identity for a root entity): the projectile moves in
        // the world, its TransformComponent stays local.
        glm::mat4 ParentWorld( entt::registry& registry, entt::entity entity )
        {
            const auto* relation = registry.try_get<RelationshipComponent>( entity );
            if ( relation == nullptr || relation->Parent == entt::null || !registry.valid( relation->Parent ) )
                return glm::mat4( 1.0f );
            return Entity( relation->Parent, registry ).GetWorldTransform();
        }

        Physics::BodyHandle BodyOf( const entt::registry& registry, entt::entity entity )
        {
            if ( entity == entt::null || !registry.valid( entity ) )
                return Physics::kInvalidBody;
            const auto* rb = registry.try_get<RigidBodyComponent>( entity );
            return rb != nullptr ? rb->RuntimeBody : Physics::kInvalidBody;
        }

        // UE bRotationFollowsVelocity: the orientation that looks along @p velocity (the engine's forward is
        // -Z, its up +Y), as the local euler angles under @p parentWorld.
        glm::vec3 RotationAlong( const glm::vec3& velocity, const glm::mat4& parentWorld )
        {
            const glm::vec3 forward = glm::normalize( velocity );
            const glm::vec3 up =
                 std::abs( forward.y ) > 0.999f ? glm::vec3( 0.0f, 0.0f, 1.0f ) : glm::vec3( 0, 1, 0 );
            const glm::quat world  = glm::quatLookAt( forward, up );
            const glm::quat parent = glm::quat_cast( glm::mat3( glm::normalize( glm::vec3( parentWorld[0] ) ),
                                                                glm::normalize( glm::vec3( parentWorld[1] ) ),
                                                                glm::normalize( glm::vec3( parentWorld[2] ) ) ) );
            return glm::eulerAngles( glm::inverse( parent ) * world );
        }

        struct Flight
        {
            entt::entity                                                 Self;
            ProjectileComponent&                                         Projectile;
            const Physics::PhysicsWorld&                                 World;
            const std::unordered_map<Physics::BodyHandle, entt::entity>& Bodies;
            std::array<Physics::BodyHandle, 2>                           Ignore;
            std::size_t                                                  IgnoreCount;
            float                                                        Gravity;
            std::vector<PhysicsEvent>&                                   Hits;
        };

        void ReportHit( Flight& flight, const Physics::RayHit& hit )
        {
            const auto         found = flight.Bodies.find( hit.Body );
            const entt::entity other = found == flight.Bodies.end() ? entt::entity( entt::null ) : found->second;
            // A sweep's hit carries no impulse (UE: NormalImpulse is zero for a swept blocking hit). Normal is
            // from Self towards Other: the surface normal faces the projectile.
            flight.Hits.push_back( { PhysicsEventKind::Hit, flight.Self, other, hit.Point, -hit.Normal, 0.0f } );
            if ( other != entt::null )
                flight.Hits.push_back(
                     { PhysicsEventKind::Hit, other, flight.Self, hit.Point, hit.Normal, 0.0f } );
        }

        // UE StopSimulating: the flight ends where it is.
        void Stop( ProjectileComponent& projectile )
        {
            projectile.Data.Velocity = glm::vec3( 0.0f );
            projectile.Stopped       = true;
        }

        // UE TickComponent's loop: @p position (world) and the velocity advanced over @p dt.
        void Fly( Flight& flight, glm::vec3& position, float dt )
        {
            ProjectileData&                            data       = flight.Projectile.Data;
            float                                      remaining  = dt;
            int                                        iterations = 0;
            int                                        impacts    = 0;
            const std::span<const Physics::BodyHandle> ignore( flight.Ignore.data(), flight.IgnoreCount );

            while ( remaining >= kMinTickTime && iterations < kMaxSimulationIterations &&
                    !flight.Projectile.Stopped )
            {
                ++iterations;
                // UE ShouldUseSubStepping: only a curved flight is cut; a straight one sweeps the whole frame.
                const bool  curved = flight.Gravity * data.GravityScale != 0.0f;
                const float tick   = curved ? SimulationTimeStep( remaining, iterations ) : remaining;
                remaining -= tick;

                const glm::vec3 oldVelocity = data.Velocity;
                const glm::vec3 delta       = ComputeMoveDelta( data, oldVelocity, flight.Gravity, tick );
                const float     length      = glm::length( delta );
                if ( length <= 0.0f )
                {
                    data.Velocity = ComputeVelocity( data, oldVelocity, flight.Gravity, tick );
                    continue;
                }
                const glm::vec3 direction = delta / length;
                const auto      hit       = flight.World.CastSphere( position, direction, data.CollisionRadius,
                                                                     length + kPullBackDistance, ignore );
                // A sphere that starts inside a surface it is leaving moves out freely (UE
                // SafeMoveUpdatedComponent resolves the penetration of a bouncing projectile).
                const bool leaving = hit && hit->Distance <= 0.0f && glm::dot( direction, hit->Normal ) > 0.0f &&
                                     data.ShouldBounce;
                if ( !hit || hit->Distance >= length || leaving )
                {
                    position += delta;
                    data.Velocity = ComputeVelocity( data, oldVelocity, flight.Gravity, tick );
                    continue;
                }

                // A blocking hit: move to it (pulled back), the velocity at the time of impact.
                const float travelled = std::max( hit->Distance - kPullBackDistance, 0.0f );
                const float time      = travelled / length;
                position += direction * travelled;
                data.Velocity = time > 1e-4f ? ComputeVelocity( data, oldVelocity, flight.Gravity, tick * time )
                                             : oldVelocity;
                ++impacts;
                ReportHit( flight, *hit );

                // UE HandleImpact: bounce, or stop.
                if ( !data.ShouldBounce )
                {
                    Stop( flight.Projectile );
                    break;
                }
                data.Velocity = ComputeBounceResult( data, data.Velocity, hit->Normal );
                if ( glm::length( data.Velocity ) < data.BounceVelocityStopSimulatingThreshold )
                {
                    Stop( flight.Projectile );
                    break;
                }
                // UE HandleDeflection: a velocity still into (or along) the surface slides along it.
                const float speed = glm::length( data.Velocity );
                if ( glm::dot( data.Velocity / speed, hit->Normal ) <= kSlideDotTolerance )
                {
                    data.Velocity -= hit->Normal * glm::dot( data.Velocity, hit->Normal );
                    if ( glm::length( data.Velocity ) < data.BounceVelocityStopSimulatingThreshold )
                    {
                        Stop( flight.Projectile );
                        break;
                    }
                }
                // The time after the impact is flown on (UE: one extra iteration for the first impacts).
                const float afterImpact = tick * ( 1.0f - time );
                if ( afterImpact >= kMinTickTime )
                {
                    remaining += afterImpact;
                    if ( impacts <= kBounceAdditionalIterations )
                        --iterations;
                }
            }
        }
    } // namespace

    glm::vec3 LimitVelocity( const ProjectileData& data, const glm::vec3& velocity )
    {
        const float speed = glm::length( velocity );
        if ( data.MaxSpeed > 0.0f && speed > data.MaxSpeed )
            return velocity * ( data.MaxSpeed / speed );
        return velocity;
    }

    glm::vec3 ComputeVelocity( const ProjectileData& data, const glm::vec3& velocity, float gravity, float dt )
    {
        const glm::vec3 acceleration( 0.0f, -gravity * data.GravityScale, 0.0f );
        return LimitVelocity( data, velocity + acceleration * dt );
    }

    glm::vec3 ComputeMoveDelta( const ProjectileData& data, const glm::vec3& velocity, float gravity, float dt )
    {
        const glm::vec3 next = ComputeVelocity( data, velocity, gravity, dt );
        return velocity * dt + ( next - velocity ) * ( 0.5f * dt );
    }

    glm::vec3 ComputeBounceResult( const ProjectileData& data, const glm::vec3& velocity, const glm::vec3& normal )
    {
        const float into = glm::dot( velocity, normal );
        if ( into > 0.0f )
            return velocity; // already leaving the surface
        const glm::vec3 projected = normal * -into;
        glm::vec3       result    = velocity + projected; // the part along the surface
        result *= std::clamp( 1.0f - data.Friction, 0.0f, 1.0f );
        result += projected * std::max( data.Bounciness, 0.0f );
        return LimitVelocity( data, result );
    }

    FrameResult Step( entt::registry& registry, const Physics::PhysicsWorld& world,
                      const std::unordered_map<Physics::BodyHandle, entt::entity>& bodies, float dt )
    {
        FrameResult result;
        if ( dt <= 0.0f )
            return result;
        const float gravity = world.GetGravity();

        auto projectiles = registry.view<TransformComponent, ProjectileComponent>();
        for ( const entt::entity entity : projectiles )
        {
            auto& projectile = projectiles.get<ProjectileComponent>( entity );
            projectile.Age += dt;
            if ( projectile.Data.LifeSpan > 0.0f && projectile.Age >= projectile.Data.LifeSpan )
            {
                result.Expired.push_back( entity );
                continue;
            }
            if ( projectile.Stopped )
                continue;

            Flight flight{ entity, projectile, world, bodies, {}, 0, gravity, result.Hits };
            for ( const entt::entity skip : { entity, projectile.Instigator } )
                if ( const Physics::BodyHandle body = BodyOf( registry, skip ); body != Physics::kInvalidBody )
                    flight.Ignore[flight.IgnoreCount++] = body;

            LocalTransform  transform   = projectiles.get<TransformComponent>( entity ).GetLocal();
            const glm::mat4 parentWorld = ParentWorld( registry, entity );
            glm::vec3       position    = glm::vec3( parentWorld * glm::vec4( transform.Translation, 1.0f ) );
            const glm::vec3 launched    = projectile.Data.Velocity;
            Fly( flight, position, dt );

            transform.Translation = glm::vec3( glm::inverse( parentWorld ) * glm::vec4( position, 1.0f ) );
            // UE: the rotation follows the velocity it flew this frame with.
            if ( projectile.Data.RotationFollowsVelocity && glm::length( launched ) > 1e-3f )
                transform.Rotation = RotationAlong( launched, parentWorld );
            SetLocalTransform( registry, entity, transform );
        }
        return result;
    }
} // namespace Desert::ECS::ProjectileMovement

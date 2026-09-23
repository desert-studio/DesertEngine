// Jolt.h MUST be the first Jolt header (it configures the rest). The engine PCH (non-Jolt) is force-
// included before this by the build, which is fine.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/RegisterTypes.h>

#include <Engine/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace Desert::Physics
{
    namespace
    {
        // Object layers: which objects can collide. Two are enough (static vs moving).
        namespace Layers
        {
            static constexpr JPH::ObjectLayer NON_MOVING = 0;
            static constexpr JPH::ObjectLayer MOVING     = 1;
            static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
        } // namespace Layers

        namespace BroadPhaseLayers
        {
            static constexpr JPH::BroadPhaseLayer NON_MOVING( 0 );
            static constexpr JPH::BroadPhaseLayer MOVING( 1 );
            static constexpr JPH::uint            NUM_LAYERS( 2 );
        } // namespace BroadPhaseLayers

        class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
        {
        public:
            BPLayerInterfaceImpl()
            {
                m_ObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
                m_ObjectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
            }
            JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer( JPH::ObjectLayer inLayer ) const override
            {
                return m_ObjectToBroadPhase[inLayer];
            }
#if defined( JPH_EXTERNAL_PROFILE ) || defined( JPH_PROFILE_ENABLED )
            const char* GetBroadPhaseLayerName( JPH::BroadPhaseLayer ) const override { return "Layer"; }
#endif
        private:
            JPH::BroadPhaseLayer m_ObjectToBroadPhase[Layers::NUM_LAYERS];
        };

        class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide( JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2 ) const override
            {
                if ( inLayer1 == Layers::NON_MOVING )
                    return inLayer2 == BroadPhaseLayers::MOVING;
                return true; // MOVING collides with everything
            }
        };

        class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide( JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2 ) const override
            {
                if ( inObject1 == Layers::NON_MOVING )
                    return inObject2 == Layers::MOVING; // static only collides with moving
                return true;                            // moving collides with everything
            }
        };

        static void TraceImpl( const char* inFMT, ... )
        {
            va_list list;
            va_start( list, inFMT );
            char buffer[1024];
            vsnprintf( buffer, sizeof( buffer ), inFMT, list );
            va_end( list );
            std::printf( "[Jolt] %s\n", buffer );
        }

        // Jolt's allocator/factory/types are PROCESS-GLOBAL. Register on the first world, unregister on
        // the last, so creating/destroying worlds (scene reloads) doesn't double-register or leak.
        int s_GlobalRefCount = 0;

        inline JPH::Vec3 ToJolt( const glm::vec3& v ) { return JPH::Vec3( v.x, v.y, v.z ); }
        inline JPH::Quat ToJolt( const glm::quat& q ) { return JPH::Quat( q.x, q.y, q.z, q.w ); }
        inline glm::vec3 ToGlm( JPH::RVec3Arg v ) { return glm::vec3( v.GetX(), v.GetY(), v.GetZ() ); }
        inline glm::quat ToGlm( JPH::QuatArg q ) { return glm::quat( q.GetW(), q.GetX(), q.GetY(), q.GetZ() ); }
    } // namespace

    struct PhysicsWorld::Impl
    {
        JPH::PhysicsSystem                  System;
        std::unique_ptr<JPH::TempAllocatorImpl>    TempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool>  JobSystem;
        BPLayerInterfaceImpl                BroadPhaseLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl   ObjectVsBroadPhaseFilter;
        ObjectLayerPairFilterImpl           ObjectLayerPairFilter;

        JPH::BodyInterface* Bodies = nullptr;

        // Character controllers (CharacterVirtual). Handle = index into this vector (nulled on remove).
        std::vector<JPH::Ref<JPH::CharacterVirtual>> Characters;
    };

    PhysicsWorld::PhysicsWorld()  = default;
    PhysicsWorld::~PhysicsWorld() { Shutdown(); }

    bool PhysicsWorld::Init( float gravityCmPerS2 )
    {
        if ( m_Impl )
            return true;

        if ( s_GlobalRefCount++ == 0 )
        {
            JPH::RegisterDefaultAllocator();
            JPH::Trace = TraceImpl;
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        m_Impl                = std::make_unique<Impl>();
        m_Impl->TempAllocator = std::make_unique<JPH::TempAllocatorImpl>( 16 * 1024 * 1024 );

        const int threads = std::max( 1u, std::thread::hardware_concurrency() - 1u );
        m_Impl->JobSystem =
             std::make_unique<JPH::JobSystemThreadPool>( JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads );

        constexpr JPH::uint kMaxBodies            = 65536;
        constexpr JPH::uint kNumBodyMutexes       = 0;
        constexpr JPH::uint kMaxBodyPairs         = 65536;
        constexpr JPH::uint kMaxContactConstraints = 16384;

        m_Impl->System.Init( kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                             m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadPhaseFilter,
                             m_Impl->ObjectLayerPairFilter );
        SetGravity( gravityCmPerS2 );
        m_Impl->Bodies = &m_Impl->System.GetBodyInterface();
        return true;
    }

    void PhysicsWorld::SetGravity( float gravityCmPerS2 )
    {
        if ( !m_Impl )
            return;

        // Down is -Y, and the magnitude arrives in centimetres per second squared because one world unit is
        // one centimetre. The value used to be the literal -981 right here, which made SceneSettings::Gravity
        // a knob that moved nothing: it was authored, saved into 45 scenes and read by no one, while 39 of
        // those scenes carried a metre-era 9.81 that was harmless only because of that.
        m_Impl->System.SetGravity( JPH::Vec3( 0.0f, -gravityCmPerS2, 0.0f ) );
    }

    void PhysicsWorld::Shutdown()
    {
        if ( !m_Impl )
            return;
        m_Impl.reset();

        if ( --s_GlobalRefCount == 0 )
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    void PhysicsWorld::Step( float dt )
    {
        if ( !m_Impl || dt <= 0.0f )
            return;

        // Fixed 60 Hz steps; clamp the backlog so a hitch can't spiral into a long catch-up.
        constexpr float kFixed = 1.0f / 60.0f;
        m_Accumulator          = std::min( m_Accumulator + dt, 0.25f );
        while ( m_Accumulator >= kFixed )
        {
            m_Impl->System.Update( kFixed, 1, m_Impl->TempAllocator.get(), m_Impl->JobSystem.get() );
            m_Accumulator -= kFixed;
        }
    }

    BodyHandle PhysicsWorld::CreateBody( const BodyDesc& desc )
    {
        if ( !m_Impl )
            return kInvalidBody;

        JPH::ShapeRefC shape;
        switch ( desc.Shape )
        {
            case ShapeType::Sphere:
                shape = new JPH::SphereShape( desc.Radius );
                break;
            case ShapeType::Capsule:
                shape = new JPH::CapsuleShape( desc.HalfHeight, desc.Radius );
                break;
            case ShapeType::Box:
            default:
                shape = new JPH::BoxShape( ToJolt( glm::max( desc.HalfExtents, glm::vec3( 1.0f ) ) ) );
                break;
        }

        const bool isStatic    = desc.Type == BodyType::Static;
        const auto motion       = desc.Type == BodyType::Dynamic     ? JPH::EMotionType::Dynamic
                                  : desc.Type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                     : JPH::EMotionType::Static;
        const JPH::ObjectLayer layer = isStatic ? Layers::NON_MOVING : Layers::MOVING;

        JPH::BodyCreationSettings settings( shape, JPH::RVec3( desc.Position.x, desc.Position.y, desc.Position.z ),
                                            ToJolt( desc.Rotation ), motion, layer );
        settings.mFriction    = desc.Friction;
        settings.mRestitution = desc.Restitution;
        if ( desc.Type == BodyType::Dynamic && desc.Mass > 0.0f )
        {
            settings.mOverrideMassProperties     = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.Mass;
        }

        const JPH::EActivation activation = isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
        const JPH::BodyID      id         = m_Impl->Bodies->CreateAndAddBody( settings, activation );
        if ( id.IsInvalid() )
            return kInvalidBody;
        return id.GetIndexAndSequenceNumber();
    }

    void PhysicsWorld::RemoveBody( BodyHandle handle )
    {
        if ( !m_Impl || handle == kInvalidBody )
            return;
        const JPH::BodyID id( handle );
        m_Impl->Bodies->RemoveBody( id );
        m_Impl->Bodies->DestroyBody( id );
    }

    glm::vec3 PhysicsWorld::GetPosition( BodyHandle handle ) const
    {
        if ( !m_Impl || handle == kInvalidBody )
            return glm::vec3( 0.0f );
        return ToGlm( m_Impl->Bodies->GetPosition( JPH::BodyID( handle ) ) );
    }

    glm::quat PhysicsWorld::GetRotation( BodyHandle handle ) const
    {
        if ( !m_Impl || handle == kInvalidBody )
            return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        return ToGlm( m_Impl->Bodies->GetRotation( JPH::BodyID( handle ) ) );
    }

    void PhysicsWorld::SetTransform( BodyHandle handle, const glm::vec3& position, const glm::quat& rotation )
    {
        if ( !m_Impl || handle == kInvalidBody )
            return;
        m_Impl->Bodies->SetPositionAndRotation( JPH::BodyID( handle ),
                                                JPH::RVec3( position.x, position.y, position.z ),
                                                ToJolt( rotation ), JPH::EActivation::Activate );
    }

    void PhysicsWorld::SetLinearVelocity( BodyHandle handle, const glm::vec3& velocity )
    {
        if ( !m_Impl || handle == kInvalidBody )
            return;
        m_Impl->Bodies->SetLinearVelocity( JPH::BodyID( handle ), ToJolt( velocity ) );
    }

    uint32_t PhysicsWorld::GetBodyCount() const
    {
        return m_Impl ? m_Impl->System.GetNumBodies() : 0u;
    }

    uint32_t PhysicsWorld::GetCharacterCount() const
    {
        if ( !m_Impl )
            return 0u;
        return static_cast<uint32_t>( std::count_if( m_Impl->Characters.begin(), m_Impl->Characters.end(),
                                                     []( const auto& c ) { return c != nullptr; } ) );
    }

    // ---- Character controller ----

    CharacterHandle PhysicsWorld::CreateCharacter( const CharacterDesc& desc )
    {
        if ( !m_Impl )
            return kInvalidCharacter;

        JPH::CharacterVirtualSettings settings;
        settings.mShape =
             new JPH::CapsuleShape( glm::max( desc.HalfHeight, 1.0f ), glm::max( desc.Radius, 1.0f ) );
        settings.mMaxSlopeAngle = glm::radians( desc.MaxSlopeDeg );
        // Keep the contact point a little inside the capsule so the character doesn't get stuck on edges.
        settings.mSupportingVolume = JPH::Plane( JPH::Vec3::sAxisY(), -desc.Radius );

        JPH::Ref<JPH::CharacterVirtual> character =
             new JPH::CharacterVirtual( &settings, ToJolt( desc.Position ), JPH::Quat::sIdentity(),
                                        &m_Impl->System );

        // Reuse a released slot before growing. Slots used to be append-only, which was harmless while
        // characters lived as long as a Play session, and is a vector that only grows once streaming creates
        // and destroys them with every cell. Reuse is safe because a slot is released only through
        // PhysicsBodyLifetime, which resets the component's handle in the same call — nothing is left
        // holding the old number.
        auto& slots = m_Impl->Characters;
        if ( const auto freeSlot = std::find( slots.begin(), slots.end(), nullptr ); freeSlot != slots.end() )
        {
            *freeSlot = character;
            return static_cast<CharacterHandle>( freeSlot - slots.begin() );
        }
        slots.push_back( character );
        return static_cast<CharacterHandle>( slots.size() - 1 );
    }

    void PhysicsWorld::RemoveCharacter( CharacterHandle handle )
    {
        if ( !m_Impl || handle >= m_Impl->Characters.size() )
            return;
        m_Impl->Characters[handle] = nullptr; // Ref release; slot kept so other handles stay valid
    }

    void PhysicsWorld::UpdateCharacter( CharacterHandle handle, const glm::vec3& velocity, float dt )
    {
        if ( !m_Impl || handle >= m_Impl->Characters.size() || !m_Impl->Characters[handle] || dt <= 0.0f )
            return;

        auto& character = m_Impl->Characters[handle];
        character->SetLinearVelocity( ToJolt( velocity ) );
        character->Update( dt, m_Impl->System.GetGravity(),
                           m_Impl->System.GetDefaultBroadPhaseLayerFilter( Layers::MOVING ),
                           m_Impl->System.GetDefaultLayerFilter( Layers::MOVING ), {}, {},
                           *m_Impl->TempAllocator );
    }

    glm::vec3 PhysicsWorld::GetCharacterPosition( CharacterHandle handle ) const
    {
        if ( !m_Impl || handle >= m_Impl->Characters.size() || !m_Impl->Characters[handle] )
            return glm::vec3( 0.0f );
        return ToGlm( m_Impl->Characters[handle]->GetPosition() );
    }

    bool PhysicsWorld::IsCharacterOnGround( CharacterHandle handle ) const
    {
        if ( !m_Impl || handle >= m_Impl->Characters.size() || !m_Impl->Characters[handle] )
            return false;
        return m_Impl->Characters[handle]->GetGroundState() ==
               JPH::CharacterBase::EGroundState::OnGround;
    }

    void PhysicsWorld::SetCharacterPosition( CharacterHandle handle, const glm::vec3& position )
    {
        if ( !m_Impl || handle >= m_Impl->Characters.size() || !m_Impl->Characters[handle] )
            return;
        m_Impl->Characters[handle]->SetPosition( ToJolt( position ) );
    }
} // namespace Desert::Physics

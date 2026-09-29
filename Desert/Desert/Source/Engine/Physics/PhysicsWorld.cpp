// Jolt.h MUST be the first Jolt header (it configures the rest). The engine PCH (non-Jolt) is force-
// included before this by the build, which is fine.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/RegisterTypes.h>

#include <Engine/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <string_view>
#include <thread>
#include <unordered_map>
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
        // Encodable height range beyond the grid's own, each side, at creation and at every rebuild. Jolt
        // quantises the whole range to 16 bits, so the headroom costs precision — (range + 2·headroom) /
        // 65534 per step, 0.05 cm on a 1000 cm hill — and buys sculpting room: an edit that stays inside
        // it patches blocks in place, one that leaves it rebuilds the shape (UpdateHeightField).
        constexpr float kHeightFieldHeadroomCm = 1000.0f;

        // Sixteen bits per sample inside a block: the block quantisation is then finer than the global
        // one, so the surface is the grid to within that global step. Eight bits (Jolt's default) would
        // put a steep 4×4 block's samples up to range / 510 off — centimetres on real slopes.
        constexpr JPH::uint32 kHeightFieldBitsPerSample = 16u;

        Common::ResultStr<JPH::Ref<JPH::HeightFieldShape>> BuildHeightField( const HeightFieldDesc& desc )
        {
            const uint32_t n = desc.SampleCount;
            if ( n % kHeightFieldBlockSize != 0u || n / kHeightFieldBlockSize < 2u )
                return Common::MakeError<JPH::Ref<JPH::HeightFieldShape>>(
                     "heightfield of " + std::to_string( n ) + " samples per side: must be a multiple of " +
                     std::to_string( kHeightFieldBlockSize ) + " and at least two blocks" );
            if ( desc.HeightsCm.size() != static_cast<size_t>( n ) * n )
                return Common::MakeError<JPH::Ref<JPH::HeightFieldShape>>(
                     "heightfield of " + std::to_string( n ) + "² samples was given " +
                     std::to_string( desc.HeightsCm.size() ) + " heights" );
            if ( !( desc.SpacingCm > 0.0f ) || !std::isfinite( desc.SpacingCm ) )
                return Common::MakeError<JPH::Ref<JPH::HeightFieldShape>>( "heightfield spacing " +
                                                                           std::to_string( desc.SpacingCm ) +
                                                                           " cm is not positive and finite" );

            const auto [low, high] = std::minmax_element( desc.HeightsCm.begin(), desc.HeightsCm.end() );
            JPH::HeightFieldShapeSettings settings( desc.HeightsCm.data(), JPH::Vec3::sZero(),
                                                    JPH::Vec3( desc.SpacingCm, 1.0f, desc.SpacingCm ), n );
            settings.mBlockSize      = kHeightFieldBlockSize;
            settings.mBitsPerSample  = kHeightFieldBitsPerSample;
            settings.mMinHeightValue = *low - kHeightFieldHeadroomCm;
            settings.mMaxHeightValue = *high + kHeightFieldHeadroomCm;

            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if ( result.HasError() )
                return Common::MakeError<JPH::Ref<JPH::HeightFieldShape>>(
                     std::string( "Jolt refused the heightfield: " ) + result.GetError().c_str() );
            return Common::MakeSuccess( JPH::Ref<JPH::HeightFieldShape>(
                 static_cast<JPH::HeightFieldShape*>( const_cast<JPH::Shape*>( result.Get().GetPtr() ) ) ) );
        }

        std::string_view ShapeName( ShapeType shape )
        {
            switch ( shape )
            {
                case ShapeType::Box:
                    return "Box";
                case ShapeType::Sphere:
                    return "Sphere";
                case ShapeType::Capsule:
                    return "Capsule";
                case ShapeType::Mesh:
                    return "Mesh";
                case ShapeType::ConvexHull:
                    return "ConvexHull";
            }
            return "unknown";
        }

        uint64_t HashBytes( uint64_t hash, const void* data, size_t size )
        {
            const auto* bytes = static_cast<const unsigned char*>( data );
            for ( size_t i = 0; i < size; ++i )
                hash = ( hash ^ bytes[i] ) * 1099511628211ull;
            return hash;
        }

        // The cache key is the CONTENT (FNV-1a over kind, points and, for Mesh, indices), not the asset that
        // supplied it: two entities on one mesh share a shape, and a mesh edited in place can never be served
        // the shape of its previous self.
        uint64_t CookKey( const BodyDesc& desc )
        {
            const auto kind = static_cast<uint32_t>( desc.Shape );
            uint64_t   hash = HashBytes( 1469598103934665603ull, &kind, sizeof( kind ) );
            hash            = HashBytes( hash, desc.MeshPoints.data(), desc.MeshPoints.size_bytes() );
            if ( desc.Shape == ShapeType::Mesh )
                hash = HashBytes( hash, desc.MeshIndices.data(), desc.MeshIndices.size_bytes() );
            return hash;
        }

        Common::ResultStr<JPH::ShapeRefC> CookConvexHull( std::span<const glm::vec3> points )
        {
            JPH::Array<JPH::Vec3> joltPoints;
            joltPoints.reserve( points.size() );
            for ( const glm::vec3& p : points )
                joltPoints.push_back( ToJolt( p ) );
            // The builder stops at cMaxPointsInHull and keeps the hull within tolerance of the rest, so a
            // dense mesh is simplified here rather than refused.
            const JPH::ConvexHullShapeSettings    settings( joltPoints );
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if ( result.HasError() )
                return Common::MakeError<JPH::ShapeRefC>( std::format(
                     "Jolt refused the convex hull of {} points: {}", points.size(), result.GetError().c_str() ) );
            return Common::MakeSuccess( JPH::ShapeRefC( result.Get() ) );
        }

        Common::ResultStr<JPH::ShapeRefC> CookTriangleMesh( std::span<const glm::vec3> points,
                                                            std::span<const uint32_t>  indices )
        {
            if ( indices.empty() || indices.size() % 3u != 0u )
                return Common::MakeError<JPH::ShapeRefC>( std::format(
                     "Mesh collider has {} indices: needs a positive multiple of three", indices.size() ) );

            JPH::VertexList vertices;
            vertices.reserve( points.size() );
            for ( const glm::vec3& p : points )
                vertices.push_back( JPH::Float3( p.x, p.y, p.z ) );
            JPH::IndexedTriangleList triangles;
            triangles.reserve( indices.size() / 3u );
            for ( size_t i = 0; i < indices.size(); i += 3u )
            {
                for ( size_t k = i; k < i + 3u; ++k )
                    if ( indices[k] >= points.size() )
                        return Common::MakeError<JPH::ShapeRefC>(
                             std::format( "Mesh collider index {} (at {}) is out of range for {} points",
                                          indices[k], k, points.size() ) );
                triangles.push_back( JPH::IndexedTriangle( indices[i], indices[i + 1u], indices[i + 2u] ) );
            }
            const JPH::MeshShapeSettings          settings( std::move( vertices ), std::move( triangles ) );
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if ( result.HasError() )
                return Common::MakeError<JPH::ShapeRefC>(
                     std::format( "Jolt refused the triangle mesh of {} points and {} triangles: {}",
                                  points.size(), indices.size() / 3u, result.GetError().c_str() ) );
            return Common::MakeSuccess( JPH::ShapeRefC( result.Get() ) );
        }
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

        // The heightfields' shapes, held non-const: SetHeights patches the shape the body already carries,
        // and the body only hands back a const reference to it.
        std::unordered_map<BodyHandle, JPH::Ref<JPH::HeightFieldShape>> HeightFields;

        // Mesh and ConvexHull shapes by CookKey: cooking a hull or a BVH is the expensive part of such a body.
        std::unordered_map<uint64_t, JPH::ShapeRefC> CookedShapes;

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

    Common::ResultStr<BodyHandle> PhysicsWorld::CreateBody( const BodyDesc& desc )
    {
        if ( !m_Impl )
            return Common::MakeError<BodyHandle>( "the physics world is not initialised" );

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
                shape = new JPH::BoxShape( ToJolt( glm::max( desc.HalfExtents, glm::vec3( 1.0f ) ) ) );
                break;
            case ShapeType::Mesh:
            case ShapeType::ConvexHull:
            {
                if ( desc.Shape == ShapeType::Mesh && desc.Type == BodyType::Dynamic )
                    return Common::MakeError<BodyHandle>(
                         "a Mesh collider cannot be on a Dynamic body: Jolt's MeshShape has no volume to carry "
                         "mass (use ConvexHull, or make the body Static)" );
                if ( desc.MeshPoints.empty() )
                    return Common::MakeError<BodyHandle>( std::format(
                         "{} collider has no points: its mesh has no vertices", ShapeName( desc.Shape ) ) );
                const uint64_t key    = CookKey( desc );
                const auto     cached = m_Impl->CookedShapes.find( key );
                if ( cached != m_Impl->CookedShapes.end() )
                {
                    shape = cached->second;
                    break;
                }
                auto cooked = desc.Shape == ShapeType::Mesh ? CookTriangleMesh( desc.MeshPoints, desc.MeshIndices )
                                                            : CookConvexHull( desc.MeshPoints );
                if ( !cooked.IsSuccess() )
                    return Common::MakeError<BodyHandle>( cooked.GetError() );
                shape = cooked.GetValue();
                m_Impl->CookedShapes.emplace( key, shape );
                break;
            }
        }

        // A simple shape off the body's origin, or a capsule off Y, is the same shape moved inside the body (UE's
        // FKShapeElem Center/Rotation). Mesh and ConvexHull points already sit where they are.
        const bool simple =
             desc.Shape == ShapeType::Box || desc.Shape == ShapeType::Sphere || desc.Shape == ShapeType::Capsule;
        const bool offAxis = desc.Shape == ShapeType::Capsule && desc.Axis != CapsuleAxis::Y;
        if ( simple && ( offAxis || desc.Center != glm::vec3( 0.0f ) ) )
        {
            // Y onto X: -90 degrees about Z; Y onto Z: +90 degrees about X.
            const JPH::Quat onto = !offAxis ? JPH::Quat::sIdentity()
                                   : desc.Axis == CapsuleAxis::X
                                        ? JPH::Quat::sRotation( JPH::Vec3::sAxisZ(), -JPH::JPH_PI * 0.5f )
                                        : JPH::Quat::sRotation( JPH::Vec3::sAxisX(), JPH::JPH_PI * 0.5f );
            shape                = new JPH::RotatedTranslatedShape( ToJolt( desc.Center ), onto, shape );
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
        // A landscape is one heightfield BODY PER TILE, and the border edges of each are active edges: a
        // ball rolling across a seam met the neighbour's edge and hopped (+0.24 cm, then sank 0.38 cm, in the
        // landscape_raycast suite). The run-time check removes that ghost contact; measured smooth to the
        // steady rolling depth. Dynamic bodies only — static and kinematic ones are never pushed by contacts.
        settings.mEnhancedInternalEdgeRemoval = desc.Type == BodyType::Dynamic;
        if ( desc.Type == BodyType::Dynamic && desc.Mass > 0.0f )
        {
            settings.mOverrideMassProperties     = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.Mass;
        }

        const JPH::EActivation activation = isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
        const JPH::BodyID      id         = m_Impl->Bodies->CreateAndAddBody( settings, activation );
        if ( id.IsInvalid() )
            return Common::MakeError<BodyHandle>(
                 std::format( "Jolt refused the {} body: {} bodies exist, the world's limit is reached",
                              ShapeName( desc.Shape ), GetBodyCount() ) );
        return Common::MakeSuccess( BodyHandle( id.GetIndexAndSequenceNumber() ) );
    }

    uint32_t PhysicsWorld::GetCookedShapeCount() const
    {
        return m_Impl ? static_cast<uint32_t>( m_Impl->CookedShapes.size() ) : 0u;
    }

    void PhysicsWorld::RemoveBody( BodyHandle handle )
    {
        if ( !m_Impl || handle == kInvalidBody )
            return;
        const JPH::BodyID id( handle );
        m_Impl->Bodies->RemoveBody( id );
        m_Impl->Bodies->DestroyBody( id );
        m_Impl->HeightFields.erase( handle );
    }

    Common::ResultStr<BodyHandle> PhysicsWorld::CreateHeightField( const HeightFieldDesc& desc )
    {
        if ( !m_Impl )
            return Common::MakeError<BodyHandle>( "the physics world is not initialised" );
        auto shape = BuildHeightField( desc );
        if ( !shape.IsSuccess() )
            return Common::MakeError<BodyHandle>( shape.GetError() );

        JPH::BodyCreationSettings settings( shape.GetValue(),
                                            JPH::RVec3( desc.Position.x, desc.Position.y, desc.Position.z ),
                                            JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING );
        settings.mFriction   = desc.Friction;
        const JPH::BodyID id = m_Impl->Bodies->CreateAndAddBody( settings, JPH::EActivation::DontActivate );
        if ( id.IsInvalid() )
            return Common::MakeError<BodyHandle>(
                 "Jolt refused the heightfield body: " + std::to_string( GetBodyCount() ) +
                 " bodies exist, the world's limit is reached" );
        const BodyHandle handle      = id.GetIndexAndSequenceNumber();
        m_Impl->HeightFields[handle] = shape.GetValue();
        return Common::MakeSuccess( BodyHandle( handle ) );
    }

    Common::ResultStr<HeightFieldUpdate> PhysicsWorld::UpdateHeightField( BodyHandle             handle,
                                                                          const HeightFieldDesc& desc, uint32_t x0,
                                                                          uint32_t z0, uint32_t x1, uint32_t z1 )
    {
        if ( !m_Impl )
            return Common::MakeError<HeightFieldUpdate>( "the physics world is not initialised" );
        const auto found = m_Impl->HeightFields.find( handle );
        if ( found == m_Impl->HeightFields.end() )
            return Common::MakeError<HeightFieldUpdate>( "body " + std::to_string( handle ) +
                                                         " is not a heightfield" );
        JPH::HeightFieldShape& shape = *found->second;
        const uint32_t         n     = desc.SampleCount;
        if ( n != shape.GetSampleCount() || desc.HeightsCm.size() != static_cast<size_t>( n ) * n )
            return Common::MakeError<HeightFieldUpdate>(
                 "heightfield " + std::to_string( handle ) + " has " + std::to_string( shape.GetSampleCount() ) +
                 " samples per side; the update carries " + std::to_string( n ) + " and " +
                 std::to_string( desc.HeightsCm.size() ) + " heights" );
        if ( !( x0 < x1 && z0 < z1 && x1 <= n && z1 <= n ) )
            return Common::MakeError<HeightFieldUpdate>(
                 "update rectangle [" + std::to_string( x0 ) + ", " + std::to_string( x1 ) + ") x [" +
                 std::to_string( z0 ) + ", " + std::to_string( z1 ) + ") is empty or leaves the " +
                 std::to_string( n ) + "-sample grid" );

        const JPH::BodyID id( handle );
        const float       lowest  = shape.GetMinHeightValue();
        const float       highest = shape.GetMaxHeightValue();
        bool              inRange = true;
        for ( uint32_t z = z0; z < z1 && inRange; ++z )
            for ( uint32_t x = x0; x < x1; ++x )
            {
                const float h = desc.HeightsCm[static_cast<size_t>( z ) * n + x];
                if ( h < lowest || h > highest )
                {
                    inRange = false;
                    break;
                }
            }

        HeightFieldUpdate done = HeightFieldUpdate::Patched;
        if ( inRange )
        {
            // Block-aligned, as Jolt requires; the widened samples are rewritten with the values they have.
            const uint32_t b  = kHeightFieldBlockSize;
            const uint32_t bx = x0 / b * b;
            const uint32_t bz = z0 / b * b;
            const uint32_t ex = std::min( ( x1 + b - 1u ) / b * b, n );
            const uint32_t ez = std::min( ( z1 + b - 1u ) / b * b, n );
            shape.SetHeights( bx, bz, ex - bx, ez - bz, desc.HeightsCm.data() + static_cast<size_t>( bz ) * n + bx,
                              static_cast<intptr_t>( n ), *m_Impl->TempAllocator );
            // The body's bounds are cached in the broad phase; the shape changing under it must be said.
            m_Impl->Bodies->NotifyShapeChanged( id, JPH::Vec3::sZero(), false, JPH::EActivation::DontActivate );
        }
        else
        {
            auto rebuilt = BuildHeightField( desc );
            if ( !rebuilt.IsSuccess() )
                return Common::MakeError<HeightFieldUpdate>( rebuilt.GetError() );
            m_Impl->Bodies->SetShape( id, rebuilt.GetValue(), false, JPH::EActivation::DontActivate );
            found->second = rebuilt.GetValue();
            done          = HeightFieldUpdate::Rebuilt;
        }

        // Wake what rests over the change: a sleeping body does not re-test its contacts on its own.
        const JPH::Vec3 lo( desc.Position.x + static_cast<float>( x0 ) * desc.SpacingCm,
                            desc.Position.y + found->second->GetMinHeightValue(),
                            desc.Position.z + static_cast<float>( z0 ) * desc.SpacingCm );
        const JPH::Vec3 hi( desc.Position.x + static_cast<float>( x1 - 1u ) * desc.SpacingCm,
                            desc.Position.y + found->second->GetMaxHeightValue(),
                            desc.Position.z + static_cast<float>( z1 - 1u ) * desc.SpacingCm );
        m_Impl->Bodies->ActivateBodiesInAABox( JPH::AABox( lo, hi ), {}, {} );
        return Common::MakeSuccess( HeightFieldUpdate( done ) );
    }

    std::optional<RayHit> PhysicsWorld::CastRay( const glm::vec3& origin, const glm::vec3& direction,
                                                 float maxDistance ) const
    {
        if ( !m_Impl || !( maxDistance > 0.0f ) || glm::length( direction ) == 0.0f )
            return std::nullopt;
        const glm::vec3     dir = glm::normalize( direction );
        const JPH::RRayCast ray( JPH::RVec3( origin.x, origin.y, origin.z ), ToJolt( dir * maxDistance ) );
        JPH::RayCastResult  result;
        if ( !m_Impl->System.GetNarrowPhaseQuery().CastRay( ray, result ) )
            return std::nullopt;

        RayHit hit;
        hit.Body               = result.mBodyID.GetIndexAndSequenceNumber();
        hit.Distance           = result.mFraction * maxDistance;
        const JPH::RVec3 point = ray.GetPointOnRay( result.mFraction );
        hit.Point              = ToGlm( point );
        JPH::BodyLockRead lock( m_Impl->System.GetBodyLockInterface(), result.mBodyID );
        if ( lock.Succeeded() )
            hit.Normal =
                 ToGlm( JPH::RVec3( lock.GetBody().GetWorldSpaceSurfaceNormal( result.mSubShapeID2, point ) ) );
        return hit;
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

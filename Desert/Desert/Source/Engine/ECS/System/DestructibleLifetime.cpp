#include <Engine/ECS/System/DestructibleLifetime.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/DestructionEvents.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <type_traits>

namespace Desert::ECS
{
    static_assert(
         std::is_same_v<decltype( DestructibleComponent::RuntimeObject ), Destruction::DestructibleHandle> );

    DestructibleLifetime::~DestructibleLifetime()
    {
        Detach();
    }

    void DestructibleLifetime::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        m_Registry = &registry;
        registry.on_destroy<DestructibleComponent>().connect<&DestructibleLifetime::OnDestructibleDestroyed>(
             *this );
    }

    void DestructibleLifetime::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<DestructibleComponent>().disconnect<&DestructibleLifetime::OnDestructibleDestroyed>(
             *this );
        // The simulation that made them is going: its events are no one's news any more.
        if ( auto* queue = m_Registry->try_ctx<DestructionEventQueue>() )
            queue->Clear();
        m_Registry = nullptr;
        m_Refused.clear();
        for ( const auto& [entity, runtime] : m_Runtime )
            m_Cuts.Cancel( runtime.Ticket );
        m_Runtime.clear();
    }

    void DestructibleLifetime::PublishEvents( entt::registry& registry ) const
    {
        auto& queue = registry.ctx_or_set<DestructionEventQueue>();
        queue.Clear();
        ++queue.Publication;
        std::unordered_map<Destruction::DestructibleHandle, DestructionEventTarget> targets;
        for ( auto entity : registry.view<DestructibleComponent>() )
        {
            const DestructibleComponent& component = registry.get<DestructibleComponent>( entity );
            targets[component.RuntimeObject]       = { entity, component.Data.NotifyBreaks,
                                                       component.Data.NotifyCollisions, component.Data.NotifyRemovals };
        }
        NameDestructionEvents( m_World->GetEvents(), targets, queue );
    }

    void DestructibleLifetime::OnDestructibleDestroyed( entt::registry& registry, entt::entity entity )
    {
        m_Refused.erase( entity ); // the id may come back as a new entity
        if ( const auto runtime = m_Runtime.find( entity ); runtime != m_Runtime.end() )
        {
            m_Cuts.Cancel( runtime->second.Ticket );
            m_Runtime.erase( runtime );
        }
        auto& destructible = registry.get<DestructibleComponent>( entity );
        destructible.RuntimeFracture.reset();
        if ( destructible.RuntimeObject == Destruction::kInvalidDestructible )
            return;
        m_World->Remove( destructible.RuntimeObject );
        destructible.RuntimeObject = Destruction::kInvalidDestructible;
        destructible.SimulatedNodeWorld.clear();
    }

    void DestructibleLifetime::WritePoses( entt::registry& registry ) const
    {
        for ( const auto entity : registry.view<DestructibleComponent>() )
        {
            auto& destructible = registry.get<DestructibleComponent>( entity );
            if ( destructible.RuntimeObject == Destruction::kInvalidDestructible )
            {
                destructible.SimulatedNodeWorld.clear();
                continue;
            }
            const size_t nodes = m_World->GetNodeCount( destructible.RuntimeObject );
            destructible.SimulatedNodeWorld.resize( nodes );
            for ( size_t node = 0; node < nodes; ++node )
                destructible.SimulatedNodeWorld[node] =
                     m_World->GetNodeWorld( destructible.RuntimeObject, static_cast<int32_t>( node ) );
        }
    }

    void DestructibleLifetime::Refuse( entt::entity entity, const std::string& reason )
    {
        m_Refused.insert( entity );
        LOG_ERROR( "[Destruction] entity {} is not destructible: {}", static_cast<uint32_t>( entity ), reason );
    }

    Common::ResultStr<Destruction::DestructibleDesc> DestructibleLifetime::DescOf( entt::registry& registry,
                                                                                   entt::entity    entity ) const
    {
        using Result = Destruction::DestructibleDesc;
        // The world pose: parents composed, as PhysicsECSSystem places a body.
        glm::mat4    world   = registry.get<TransformComponent>( entity ).GetTransform();
        entt::entity current = entity;
        while ( registry.has<RelationshipComponent>( current ) )
        {
            const auto& relationship = registry.get<RelationshipComponent>( current );
            if ( relationship.Parent == entt::null )
                break;
            current = relationship.Parent;
            if ( registry.has<TransformComponent>( current ) )
                world = registry.get<TransformComponent>( current ).GetTransform() * world;
        }
        glm::mat3       basis( world );
        const glm::vec3 scale( glm::length( basis[0] ), glm::length( basis[1] ), glm::length( basis[2] ) );
        if ( glm::any( glm::greaterThan( glm::abs( scale - glm::vec3( 1.0f ) ), glm::vec3( 1e-3f ) ) ) )
            return Common::MakeError<Result>(
                 std::format( "its world scale is ({}, {}, {}); a fracture is simulated "
                              "at the size it was cut, so the scale must be one",
                              scale.x, scale.y, scale.z ) );

        const DestructibleData&       data = registry.get<DestructibleComponent>( entity ).Data;
        Destruction::DestructibleDesc desc;
        desc.Position                             = glm::vec3( world[3] );
        desc.Rotation                             = glm::quat_cast( basis );
        desc.AnchoredNodes                        = data.AnchoredNodes;
        desc.DamageThreshold                      = data.DamageThreshold;
        desc.Settings.DensityKgPerCm3             = data.DensityKgPerCm3;
        desc.Settings.Friction                    = data.Friction;
        desc.Settings.Restitution                 = data.Restitution;
        desc.Settings.RemoveOnSleep               = data.RemoveOnSleep;
        desc.Settings.MaxSleepTime                = data.MaxSleepTime;
        desc.Settings.SlowMovingAsSleeping        = data.SlowMovingAsSleeping;
        desc.Settings.SlowMovingVelocityThreshold = data.SlowMovingVelocityThreshold;
        // Collision events are recorded only for the objects that publish them (DestructionEvents.hpp).
        desc.Settings.CollisionEvents          = data.NotifyCollisions;
        desc.Settings.CollisionEventMinImpulse = data.CollisionEventMinImpulse;
        return Common::MakeSuccess( std::move( desc ) );
    }

    Common::ResultStr<Destruction::RuntimeFractureRequest>
    DestructibleLifetime::RuntimeSource( entt::registry& registry, entt::entity entity,
                                         const MeshFileLookup& meshFile ) const
    {
        using Result     = Destruction::RuntimeFractureRequest;
        const auto* mesh = registry.try_get<StaticMeshComponent>( entity );
        if ( mesh == nullptr )
            return Common::MakeError<Result>(
                 "a run-time Fracture Source cuts the entity's static mesh, and it has "
                 "none" );
        Result                  request;
        const DestructibleData& data = registry.get<DestructibleComponent>( entity ).Data;
        if ( data.InteriorMaterial )
            if ( const auto guid =
                      Assets::ContentRegistry::GuidForHandle( static_cast<uint64_t>( data.InteriorMaterial ) ) )
                request.InteriorMaterial = *guid;

        // The geometry the entity draws, in the mesh draw's order (MeshECSSystem): an edit, a primitive, an asset.
        if ( mesh->EditableMesh )
        {
            std::shared_ptr<const Geometry::DynamicMesh3> editable = mesh->EditableMesh;
            request.SourceKey  = std::format( "edit:{}", static_cast<const void*>( editable.get() ) );
            request.MakeSource = [editable] { return Common::MakeSuccess( editable ); };
            return Common::MakeSuccess( std::move( request ) );
        }
        if ( mesh->Primitive.has_value() )
        {
            const Geometry::PrimitiveType type = *mesh->Primitive;
            request.SourceKey  = std::format( "primitive:{}", Geometry::PrimitiveTypeName( type ) );
            request.MakeSource = [type]() -> Common::ResultStr<std::shared_ptr<const Geometry::DynamicMesh3>>
            {
                using Made       = std::shared_ptr<const Geometry::DynamicMesh3>;
                const auto shape = Geometry::MakePrimitive( type );
                if ( !shape )
                    return Common::MakeFormattedError<Made>( "the {} primitive has no mesh",
                                                             Geometry::PrimitiveTypeName( type ) );
                auto lifted = Geometry::ShapeToDynamicMesh( *shape );
                if ( !lifted )
                    return Common::MakeError<Made>( lifted.GetError() );
                return Common::MakeSuccess(
                     Made( std::make_shared<Geometry::DynamicMesh3>( lifted.ExtractValue() ) ) );
            };
            return Common::MakeSuccess( std::move( request ) );
        }
        if ( !mesh->MeshHandle )
            return Common::MakeError<Result>( "its static mesh is unset" );
        const std::filesystem::path file = meshFile ? meshFile( mesh->MeshHandle ) : std::filesystem::path();
        if ( file.empty() )
            return Common::MakeFormattedError<Result>( "its static mesh asset {} is not loaded",
                                                       static_cast<uint64_t>( mesh->MeshHandle ) );
        if ( const auto guid =
                  Assets::ContentRegistry::GuidForHandle( static_cast<uint64_t>( mesh->MeshHandle ) ) )
            request.SourceMesh = *guid;
        request.SourceKey  = file.string();
        request.MakeSource = [file]() -> Common::ResultStr<std::shared_ptr<const Geometry::DynamicMesh3>>
        {
            // The loader's read (the render form from the DDC), as the Fracture mode's Generate lifts it.
            using Made     = std::shared_ptr<const Geometry::DynamicMesh3>;
            const auto raw = Assets::LoadMeshPlatformData( file );
            if ( !raw )
                return Common::MakeError<Made>( raw.GetError() );
            auto data = Assets::Serialization::ReadMeshAssetData( raw.GetValue(), file.string() );
            if ( !data )
                return Common::MakeError<Made>( data.GetError() );
            auto lifted = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
            if ( !lifted )
                return Common::MakeError<Made>( lifted.GetError() );
            return Common::MakeSuccess(
                 Made( std::make_shared<Geometry::DynamicMesh3>( std::move( lifted.ExtractValue().Mesh ) ) ) );
        };
        return Common::MakeSuccess( std::move( request ) );
    }

    void DestructibleLifetime::Sync( entt::registry& registry, const FractureLookup& lookup,
                                     const MeshFileLookup& meshFile )
    {
        const auto now = std::chrono::steady_clock::now();
        if ( m_LastSync != std::chrono::steady_clock::time_point{} )
        {
            const double ms = std::chrono::duration<double, std::milli>( now - m_LastSync ).count();
            if ( m_Cuts.Pending() > 0 )
            {
                ++m_FramesCutting;
                m_CuttingMsSum += ms;
                m_CuttingMsMax = std::max( m_CuttingMsMax, ms );
            }
            else
            {
                ++m_FramesIdle;
                m_IdleMsSum += ms;
            }
        }
        m_LastSync = now;
        SwapStruck( registry );

        auto view = registry.view<TransformComponent, DestructibleComponent>();
        for ( const auto entity : view )
        {
            auto& destructible = view.get<DestructibleComponent>( entity );
            if ( destructible.RuntimeObject != Destruction::kInvalidDestructible || m_Refused.contains( entity ) )
                continue;
            const DestructibleData& data = destructible.Data;

            std::shared_ptr<const Destruction::FractureData> fracture;
            if ( data.Source == FractureSource::Baked )
            {
                if ( data.Fracture == Assets::AssetHandle::Null() )
                {
                    Refuse( entity, "its Rest Collection is empty: drag a .dfrac onto it" );
                    continue;
                }
                auto read = lookup( data.Fracture );
                if ( !read.IsSuccess() )
                {
                    Refuse( entity, read.GetError() );
                    continue;
                }
                fracture = read.GetValue();
            }
            else
            {
                auto [state, fresh] = m_Runtime.try_emplace( entity );
                if ( fresh )
                {
                    auto      source = RuntimeSource( registry, entity, meshFile );
                    const int sites  = data.Source == FractureSource::OnLoad
                                            ? data.RuntimeSites
                                            : data.ImpactShells * data.ImpactSitesPerShell;
                    if ( source.IsSuccess() && ( sites < 1 || sites > Destruction::kMaxRuntimeSites ) )
                        source = Common::MakeFormattedError<Destruction::RuntimeFractureRequest>(
                             "its run-time cut asks for {} sites; 1 to {} are allowed", sites,
                             Destruction::kMaxRuntimeSites );
                    if ( !source.IsSuccess() )
                    {
                        m_Runtime.erase( state );
                        Refuse( entity, source.GetError() );
                        continue;
                    }
                    RuntimeState& runtime = state->second;
                    runtime.Source        = source.ExtractValue();
                    // The instance's seed: its UUID, the identity that survives save, load and the network.
                    const uint64_t uuid =
                         registry.has<UUIDComponent>( entity )
                              ? static_cast<uint64_t>( registry.get<UUIDComponent>( entity ).UUID )
                              : static_cast<uint64_t>( static_cast<uint32_t>( entity ) );
                    runtime.Seed = Destruction::MixSeed( uuid, 0 );
                    // A level-1 piece breaks off at the component's level-1 threshold, else the cut's own.
                    runtime.TriggerImpulse = data.DamageThreshold.size() > 1
                                                  ? data.DamageThreshold[1]
                                                  : Destruction::FractureLevelSettings{}.DamageThreshold;

                    Destruction::RuntimeFractureRequest request = runtime.Source;
                    request.Settings.Seed                       = runtime.Seed;
                    if ( data.Source == FractureSource::OnLoad )
                    {
                        Destruction::FractureLevelSettings level;
                        level.Method    = Destruction::FractureMethod::Uniform;
                        level.SiteCount = data.RuntimeSites;
                        request.Settings.Levels.push_back( level );
                    }
                    // At Impact: no level — the whole, one node, until the hit.
                    runtime.Ticket = m_Cuts.Submit( std::move( request ) );
                }
                RuntimeState& runtime = state->second;
                if ( runtime.Stage != RuntimeState::StageKind::Cutting )
                    continue; // simulated before and gone (removed on sleep): not made again
                auto cut = m_Cuts.Poll( runtime.Ticket );
                if ( !cut.IsSuccess() )
                {
                    runtime.Stage = RuntimeState::StageKind::Done;
                    Refuse( entity, std::format( "its run-time fracture failed: {}", cut.GetError() ) );
                    continue;
                }
                if ( !cut.GetValue().Data )
                    continue; // still cutting: the frame does not wait
                fracture = cut.GetValue().Data;
                LOG_INFO( "[Destruction] entity {} {}: {} nodes cut on a worker in {:.2f} ms; {}",
                          static_cast<uint32_t>( entity ),
                          data.Source == FractureSource::OnLoad ? "On Load" : "At Impact (whole)",
                          fracture->Nodes.size(), cut.GetValue().BakeMs, FramesWhileCutting() );
                runtime.Ticket = 0;
                runtime.Stage  = data.Source == FractureSource::OnLoad ? RuntimeState::StageKind::Done
                                                                       : RuntimeState::StageKind::Whole;
            }
            if ( !fracture )
                continue; // still loading

            auto desc = DescOf( registry, entity );
            if ( !desc.IsSuccess() )
            {
                Refuse( entity, desc.GetError() );
                continue;
            }
            Destruction::DestructibleDesc placed = desc.ExtractValue();
            if ( data.Source == FractureSource::AtImpact )
            {
                // The whole records the contacts that may cut it, from the trigger up (and from the component's
                // own minimum when it publishes them).
                placed.Settings.CollisionEventMinImpulse =
                     data.NotifyCollisions
                          ? std::min( data.CollisionEventMinImpulse, m_Runtime[entity].TriggerImpulse )
                          : m_Runtime[entity].TriggerImpulse;
                placed.Settings.CollisionEvents = true;
            }
            auto added = m_World->Add( fracture, placed );
            if ( !added.IsSuccess() )
            {
                Refuse( entity, added.GetError() );
                continue;
            }
            destructible.RuntimeObject = added.GetValue();
            if ( data.Source != FractureSource::Baked )
                destructible.RuntimeFracture = fracture;
        }
    }

    void DestructibleLifetime::CutOnImpact( entt::registry& registry )
    {
        struct Hit
        {
            entt::entity Entity  = entt::null;
            glm::vec3    Point   = glm::vec3( 0.0f );
            float        Impulse = 0.0f;
        };
        std::unordered_map<Destruction::DestructibleHandle, Hit> wholes;
        for ( const auto& [entity, runtime] : m_Runtime )
            if ( runtime.Stage == RuntimeState::StageKind::Whole && registry.valid( entity ) &&
                 registry.has<DestructibleComponent>( entity ) )
            {
                const auto object = registry.get<DestructibleComponent>( entity ).RuntimeObject;
                if ( object != Destruction::kInvalidDestructible )
                    wholes[object].Entity = entity;
            }
        if ( wholes.empty() )
            return;
        for ( const Destruction::DestructionEvent& event : m_World->GetEvents() )
        {
            if ( event.Kind != Destruction::DestructionEventKind::Collision )
                continue;
            const auto found = wholes.find( event.Object );
            if ( found == wholes.end() || event.Impulse < m_Runtime[found->second.Entity].TriggerImpulse ||
                 event.Impulse <= found->second.Impulse )
                continue;
            found->second.Point   = event.Position;
            found->second.Impulse = event.Impulse;
        }
        for ( const auto& [object, hit] : wholes )
        {
            if ( hit.Impulse <= 0.0f )
                continue;
            const auto pose = m_World->GetNodeWorld( object, 0 );
            if ( !pose )
                continue;
            RuntimeState&           runtime = m_Runtime[hit.Entity];
            const DestructibleData& data    = registry.get<DestructibleComponent>( hit.Entity ).Data;
            runtime.HitLocal                = glm::vec3( glm::inverse( *pose ) * glm::vec4( hit.Point, 1.0f ) );
            runtime.HitImpulse              = hit.Impulse;

            Destruction::FractureLevelSettings level;
            level.Method              = Destruction::FractureMethod::Radial;
            level.Radial.Center       = glm::dvec3( runtime.HitLocal );
            level.Radial.Radius       = data.ImpactRadius;
            level.Radial.RadialSteps  = data.ImpactShells;
            level.Radial.AngularSteps = data.ImpactSitesPerShell;
            // The seed: the instance's, and the hit to the millimetre in the object's own space — the same hit
            // on the same object cuts the same pieces on every machine.
            const glm::i64vec3 mm( glm::round( glm::dvec3( runtime.HitLocal ) * 10.0 ) );
            uint64_t           seed = runtime.Seed;
            for ( int axis = 0; axis < 3; ++axis )
                seed = Destruction::MixSeed( seed, static_cast<uint64_t>( mm[axis] ) );

            Destruction::RuntimeFractureRequest request = runtime.Source;
            request.Settings.Seed                       = seed;
            request.Settings.Levels                     = { level };
            runtime.Ticket                              = m_Cuts.Submit( std::move( request ) );
            runtime.Stage                               = RuntimeState::StageKind::Struck;
            LOG_INFO( "[Destruction] entity {} hit at ({:.1f}, {:.1f}, {:.1f}) cm with {:.0f} kg*cm/s: cutting "
                      "around it",
                      static_cast<uint32_t>( hit.Entity ), hit.Point.x, hit.Point.y, hit.Point.z, hit.Impulse );
        }
    }

    std::string DestructibleLifetime::FramesWhileCutting()
    {
        const std::string said =
             m_FramesCutting == 0
                  ? std::string( "no frame ran while cutting" )
                  : std::format( "{} frame(s) ran while cutting, {:.1f} ms average, {:.1f} ms longest",
                                 m_FramesCutting, m_CuttingMsSum / m_FramesCutting, m_CuttingMsMax ) +
                         ( m_FramesIdle == 0 ? std::string()
                                             : std::format( " (frames with no cut so far: {:.1f} ms average)",
                                                            m_IdleMsSum / m_FramesIdle ) );
        m_FramesCutting = 0;
        m_CuttingMsSum  = 0.0;
        m_CuttingMsMax  = 0.0;
        return said;
    }

    void DestructibleLifetime::SwapStruck( entt::registry& registry )
    {
        for ( auto& [entity, runtime] : m_Runtime )
        {
            if ( runtime.Stage != RuntimeState::StageKind::Struck )
                continue;
            auto cut = m_Cuts.Poll( runtime.Ticket );
            if ( cut.IsSuccess() && !cut.GetValue().Data )
                continue; // still cutting: the whole goes on as it is
            runtime.Ticket     = 0;
            runtime.Stage      = RuntimeState::StageKind::Done;
            auto& destructible = registry.get<DestructibleComponent>( entity );
            if ( !cut.IsSuccess() )
            {
                LOG_ERROR( "[Destruction] entity {} stays whole: its cut at the hit failed: {}",
                           static_cast<uint32_t>( entity ), cut.GetError() );
                continue;
            }
            const auto old  = destructible.RuntimeObject;
            const auto pose = m_World->GetNodeWorld( old, 0 );
            if ( !pose )
                continue; // the whole is gone (removed) while it was being cut
            const auto [linear, angular] = m_World->GetNodeVelocity( old, 0 );

            auto desc = DescOf( registry, entity );
            if ( !desc.IsSuccess() )
                continue; // placed once, so DescOf answered then; the settings it reads cannot have changed
            Destruction::DestructibleDesc placed = desc.ExtractValue();
            placed.Position                      = glm::vec3( ( *pose )[3] );
            placed.Rotation                      = glm::quat_cast( glm::mat3( *pose ) );
            placed.LinearVelocity                = linear;
            placed.AngularVelocity               = angular;

            const auto swapBegan = std::chrono::steady_clock::now();
            m_World->Remove( old );
            destructible.RuntimeObject = Destruction::kInvalidDestructible;
            destructible.RuntimeFracture.reset();
            destructible.SimulatedNodeWorld.clear();
            const auto fracture = cut.GetValue().Data;
            auto       added    = m_World->Add( fracture, placed );
            if ( !added.IsSuccess() )
            {
                Refuse( entity, added.GetError() );
                continue;
            }
            destructible.RuntimeObject   = added.GetValue();
            destructible.RuntimeFracture = fracture;

            // The hit's impulse as strain on the cut, falling off over the impact radius from where the hit is
            // now.
            Destruction::RadialFalloff strain;
            strain.Magnitude = runtime.HitImpulse;
            strain.Radius    = destructible.Data.ImpactRadius;
            strain.Position  = glm::vec3( *pose * glm::vec4( runtime.HitLocal, 1.0f ) );
            Destruction::FieldCommand command;
            command.Type        = Destruction::FieldPhysicsType::ExternalStrain;
            command.Scalar      = strain;
            const uint32_t hits = m_World->ApplyField( command );
            const double   swapMs =
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - swapBegan ).count();
            LOG_INFO( "[Destruction] entity {} At Impact: swapped in on the main thread in {:.2f} ms at ({:.0f}, "
                      "{:.0f}, "
                      "{:.0f}) cm/s, ({:.2f}, {:.2f}, {:.2f}) rad/s; {}",
                      static_cast<uint32_t>( entity ), swapMs, linear.x, linear.y, linear.z, angular.x, angular.y,
                      angular.z, FramesWhileCutting() );
            LOG_INFO( "[Destruction] entity {} At Impact: {} nodes cut on a worker in {:.2f} ms, swapped in; the "
                      "hit's strain acted on {} bodies",
                      static_cast<uint32_t>( entity ), fracture->Nodes.size(), cut.GetValue().BakeMs, hits );
        }
    }
} // namespace Desert::ECS

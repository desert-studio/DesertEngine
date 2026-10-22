#include "VFXWorld.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXRandom.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/VFX/VFXSystemService.hpp>
#include <Engine/Assets/VFXEffectTypeAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <algorithm>
#include <limits>

#include <glm/geometric.hpp>
#include <unordered_map>

namespace Desert::VFX
{
    void VFXWorld::ResetInstance( EmitterInstance& instance )
    {
        instance.Generation = ++m_LastGeneration;
        instance.NextId     = 0;
        instance.Spawn      = {};
    }

    void VFXWorld::Clear()
    {
        m_Clock = Clock( m_Clock.GetSettings() );
        m_Plan  = {};
        m_Systems.clear();
        m_GpuState.reset(); // the scene's GPU particle state goes with its instances
        m_Channels.Clear();
        // m_LastGeneration is NOT reset: a renderer may still hold GPU state stamped with an old
        // generation, and a fresh instance must never be mistaken for it.
    }

    const SystemInstance* VFXWorld::FindSystem( std::uint64_t entityUuid ) const
    {
        const auto it = m_Systems.find( entityUuid );
        return it != m_Systems.end() ? &it->second : nullptr;
    }

    void PlanEmitterSteps( EmitterInstance& instance, const VFXSpawnPlan& plan, const std::uint32_t stepCount,
                           const double stepSeconds, const VFXDataChannels& channels, const glm::vec3& emitterCm )
    {
        instance.Steps.clear();
        instance.ChannelSpawns.clear();
        instance.ChannelReport = {};

        VFXChannelSpawnBatch batch;
        if ( plan.Channel )
        {
            batch = GatherChannelSpawns( *plan.Channel, channels.Find( plan.Channel->Channel ), emitterCm );
            if ( stepCount == 0 )
            {
                // A paused tick runs no step: the entries are cleared with the channel, so they are counted.
                batch.Report.Overflow += batch.Report.Spawned;
                batch.Report.Spawned = 0;
                batch.Requests.clear();
            }
            instance.ChannelReport = batch.Report;
            instance.ChannelOverflowTotal += batch.Report.Overflow;
        }
        const std::uint32_t channelParticles = batch.ParticleCount();

        instance.Steps.reserve( stepCount );
        for ( std::uint32_t s = 0; s < stepCount; ++s )
        {
            EmitterStep step{ instance.NextId, instance.Spawn.Step( plan, stepSeconds, instance.SpawnCountScale ) };
            if ( s == 0 )
            {
                step.ChannelFirst = 0;
                step.ChannelCount = channelParticles;
                step.Budget += channelParticles;
            }
            instance.Steps.push_back( step );
            instance.NextId += step.Budget;
        }
        instance.ChannelSpawns = std::move( batch.Requests );
    }

    void VFXWorld::Tick( entt::registry& registry, double seconds )
    {
        ++m_TickSerial;
        m_Plan = m_Clock.Advance( seconds );

        if ( m_Plan.Reset )
            for ( auto& entry : m_Systems )
                for ( EmitterInstance& emitter : entry.second.Emitters )
                    ResetInstance( emitter );

        const double stepSeconds = m_Clock.GetSettings().StepSeconds;

        for ( auto& entry : m_Systems )
            entry.second.Seen = false;

        // THE SCALABILITY DECISION (UE FNiagaraScalabilityManager, per world and per EFFECT TYPE): the views
        // reported since the last tick, each reading its Effects level of the system's type. The most demanding
        // view wins the spawn scale; a system is culled only when it is past its type's MaxDistance at EVERY
        // view's level (a level with MaxDistance 0 culls nothing). No view (headless) = the system as authored.
        std::vector<VFXViewReport> views;
        views.swap( m_PendingViews );
        using Level        = Assets::Serialization::VFXEffectTypeLevel;
        const auto levelOf = []( const Assets::Serialization::VFXEffectTypeData& type,
                                 const int                                       level ) -> const Level&
        {
            return type.Levels[static_cast<std::size_t>( std::clamp(
                 level, 0, static_cast<int>( Assets::Serialization::kVFXEffectTypeLevelCount ) - 1 ) )];
        };
        const auto spawnScaleOf = [&views, &levelOf]( const Assets::Serialization::VFXEffectTypeData& type )
        {
            double scale = views.empty() ? 1.0 : 0.0;
            for ( const VFXViewReport& v : views )
                scale = std::max( scale, v.SpawnCountScale * levelOf( type, v.EffectsLevel ).SpawnCountScale );
            return scale;
        };
        const auto outOfRange = [&views, &levelOf]( const Assets::Serialization::VFXEffectTypeData& type,
                                                    const glm::vec3& at, float& nearestCm )
        {
            nearestCm   = std::numeric_limits<float>::infinity();
            bool culled = !views.empty();
            for ( const VFXViewReport& v : views )
            {
                const float d   = glm::distance( at, v.Origin );
                const float max = levelOf( type, v.EffectsLevel ).MaxDistance;
                nearestCm       = std::min( nearestCm, d );
                culled          = culled && max > 0.0f && d > max;
            }
            return culled;
        };
        m_CulledSystems = 0;

        Runtime::VFXSystemService* systems = Runtime::ResourceRegistry::GetVFXSystemService();
        auto                       view    = registry.view<ECS::VFXComponent, ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, ECS::VFXComponent& vfx, const ECS::UUIDComponent& id )
             {
                 if ( static_cast<uint64_t>( vfx.Data.System ) == 0 )
                     return; // an empty slot plays nothing
                 // Pending = not yet read (the system starts when it lands); a failed read is logged once by the
                 // service, naming the handle.
                 const auto resolved = systems->Get( vfx.Data.System );
                 if ( !resolved.IsSuccess() || !resolved.GetValue() )
                     return;
                 const auto& data = resolved.GetValue();

                 const std::uint64_t uuid   = id.UUID;
                 SystemInstance&     system = m_Systems[uuid];
                 if ( system.System != data )
                 {
                     // A new system (first sight, renamed, reloaded): every emitter starts over from its plan.
                     const std::size_t count = data->Emitters.size();
                     system.System           = data;
                     system.Plans.assign( count, {} );
                     system.Runs.assign( count, false );
                     system.Emitters.assign( count, {} );
                     for ( std::size_t k = 0; k < count; ++k )
                     {
                         auto plan = CompileSpawnPlan( *data, k );
                         if ( !plan.IsSuccess() )
                         {
                             LOG_ERROR( "[VFX] entity {} emitter {} '{}' bears nothing: {}", uuid, k,
                                        data->Emitters[k].Name, plan.GetError() );
                         }
                         else
                         {
                             system.Plans[k] = plan.GetValue();
                             system.Runs[k]  = data->Emitters[k].Enabled;
                         }
                         system.Emitters[k].Seed =
                              MakeEmitterSeed( data->Seed, uuid, static_cast<std::uint32_t>( k ) );
                         ResetInstance( system.Emitters[k] );
                     }
                 }
                 system.Seen = true;

                 // The editor's Restart: this system alone starts over.
                 if ( vfx.RequestRestart )
                 {
                     vfx.RequestRestart = false;
                     for ( EmitterInstance& emitter : system.Emitters )
                         ResetInstance( emitter );
                 }

                 const auto*     transform = registry.try_get<ECS::TransformComponent>( entity );
                 const glm::vec3 emitterCm =
                      transform ? glm::vec3( transform->GetTransform()[3] ) : glm::vec3( 0.0f );
                 // The system's effect type (ValidateVFXSystemData requires one); a missing type spawns nothing.
                 const std::string&                              typeName  = data->EffectType.value();
                 const Assets::Serialization::VFXEffectTypeData* type      = EffectType( typeName );
                 float      nearestCm = 0.0f;
                 const bool culled = !type || outOfRange( *type, emitterCm, nearestCm );
                 if ( type && culled != system.Culled )
                     LOG_INFO( "[VFX] entity {} {}: nearest view {:.0f} cm (effect type '{}' MaxDistance)", uuid,
                               culled ? "culled, spawning stops" : "back in range, spawning resumes", nearestCm,
                               typeName );
                 system.Culled                = culled;
                 const double spawnCountScale = type ? spawnScaleOf( *type ) : 0.0;
                 m_CulledSystems += culled ? 1u : 0u;
                 for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                 {
                     EmitterInstance& emitter = system.Emitters[k];
                     if ( !system.Runs[k] || !vfx.Data.AutoActivate || system.Culled )
                     {
                         // Not active (UE bAutoActivate false), refused or distance-culled: no births, the
                         // instance is kept (its live particles finish).
                         emitter.Steps.clear();
                         emitter.ChannelSpawns.clear();
                         continue;
                     }
                     emitter.SpawnCountScale = spawnCountScale;
                     PlanEmitterSteps( emitter, system.Plans[k], m_Plan.StepCount, stepSeconds, m_Channels,
                                       emitterCm );
                 }
             } );

        std::erase_if( m_Systems, []( const auto& entry ) { return !entry.second.Seen; } );
        m_Channels.ClearEntries(); // a channel holds one frame of entries (UE: a data channel is cleared per tick)
    }

    const Assets::Serialization::VFXEffectTypeData* VFXWorld::EffectType( const std::string& name )
    {
        if ( const auto found = m_EffectTypes.find( name ); found != m_EffectTypes.end() )
            return found->second ? &found->second->GetData() : nullptr;
        auto& slot     = m_EffectTypes[name];
        auto  resolved = Runtime::ResourceRegistry::GetVFXSystemService()->EffectType( name );
        if ( !resolved )
        {
            LOG_ERROR( "[VFX] effect type '{}' is not usable - its systems spawn nothing: {}", name,
                       resolved.GetError() );
            return nullptr;
        }
        slot = resolved.ExtractValue();
        return &slot->GetData();
    }
} // namespace Desert::VFX

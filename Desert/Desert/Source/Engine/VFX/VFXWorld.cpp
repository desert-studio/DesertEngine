#include "VFXWorld.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXRandom.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
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

    VFXDebugStats VFXWorld::GetDebugStats() const
    {
        VFXDebugStats stats;
        stats.TickSerial = m_TickSerial;
        stats.Step       = m_Clock.GetStep();
        stats.Frozen     = m_DebugSettings.Frozen;
        stats.Systems.reserve( m_Systems.size() );
        for ( const auto& [uuid, system] : m_Systems )
        {
            VFXSystemDebugStats& out = stats.Systems.emplace_back();
            out.EntityUuid           = uuid;
            out.EntityId             = static_cast<std::uint32_t>( system.Entity );
            out.Transform            = system.Transform;
            if ( !system.System )
                continue;
            out.Bounds = system.System->Bounds;
            out.Emitters.reserve( system.Emitters.size() );
            for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
            {
                const EmitterInstance&                       instance = system.Emitters[k];
                const Assets::Serialization::VFXEmitterData& data     = system.System->Emitters[k];
                VFXEmitterDebugStats&                        e        = out.Emitters.emplace_back();
                e.Emitter  = static_cast<std::uint32_t>( k );
                e.Name     = data.Name;
                e.Runs     = system.Runs[k];
                e.Capacity = data.Enabled ? static_cast<std::uint32_t>( data.Capacity ) : 0u;
                e.Steps    = static_cast<std::uint32_t>( instance.Steps.size() );
                for ( const EmitterStep& step : instance.Steps )
                    e.Spawned += step.Budget;
                e.NextId               = instance.NextId;
                e.Channel              = instance.ChannelReport;
                e.ChannelOverflowTotal = instance.ChannelOverflowTotal;
            }
        }
        std::sort( stats.Systems.begin(), stats.Systems.end(),
                   []( const VFXSystemDebugStats& a, const VFXSystemDebugStats& b )
                   { return a.EntityUuid < b.EntityUuid; } );
        if ( m_GpuState )
            m_GpuState->FillDebugStats( stats );
        return stats;
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
            EmitterStep step{ instance.NextId, instance.Spawn.Step( plan, stepSeconds ) };
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
        if ( m_DebugSettings.Frozen )
        {
            // The debugger's pause: no time passes but the steps it asked for. One StepSeconds after a remainder
            // below one step is exactly one step.
            seconds = m_FrozenSteps > 0 ? m_Clock.GetSettings().StepSeconds : 0.0;
            if ( m_FrozenSteps > 0 )
                --m_FrozenSteps;
        }
        m_Plan = m_Clock.Advance( seconds );

        if ( m_Plan.Reset )
            for ( auto& entry : m_Systems )
                for ( EmitterInstance& emitter : entry.second.Emitters )
                    ResetInstance( emitter );

        const double stepSeconds = m_Clock.GetSettings().StepSeconds;

        for ( auto& entry : m_Systems )
            entry.second.Seen = false;

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
                 system.Seen   = true;
                 system.Entity = entity;

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
                 system.Transform = transform ? transform->GetTransform() : glm::mat4( 1.0f );
                 for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                 {
                     EmitterInstance& emitter = system.Emitters[k];
                     if ( !system.Runs[k] || !vfx.Data.AutoActivate )
                     {
                         // Not active (UE bAutoActivate false) or refused: no births, the instance is kept.
                         emitter.Steps.clear();
                         emitter.ChannelSpawns.clear();
                         continue;
                     }
                     PlanEmitterSteps( emitter, system.Plans[k], m_Plan.StepCount, stepSeconds, m_Channels,
                                       emitterCm );
                 }
             } );

        std::erase_if( m_Systems, []( const auto& entry ) { return !entry.second.Seen; } );
        m_Channels.ClearEntries(); // a channel holds one frame of entries (UE: a data channel is cleared per tick)
    }
} // namespace Desert::VFX

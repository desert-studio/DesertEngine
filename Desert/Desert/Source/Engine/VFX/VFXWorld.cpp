#include "VFXWorld.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXRandom.hpp>

#include <algorithm>
#include <unordered_map>

namespace Desert::VFX
{
    namespace
    {
        // A ParticleEmitterComponent is a system of ONE emitter that has no seed field of its own, so its
        // seed is the entity's identity alone (system seed 0, emitter index 0). The `.dfx` system asset
        // (VFX-02) carries the system seed and the emitter index for real.
        constexpr std::uint32_t kComponentSystemSeed   = 0;
        constexpr std::uint32_t kComponentEmitterIndex = 0;
    } // namespace

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
        m_Emitters.clear();
        m_GpuState.reset(); // the scene's GPU particle state goes with its instances
        m_Channels.Clear();
        // m_LastGeneration is NOT reset: a renderer may still hold GPU state stamped with an old
        // generation, and a fresh instance must never be mistaken for it.
    }

    const EmitterInstance* VFXWorld::FindEmitter( std::uint64_t entityUuid ) const
    {
        const auto it = m_Emitters.find( entityUuid );
        return it != m_Emitters.end() ? &it->second : nullptr;
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
        m_Plan = m_Clock.Advance( seconds );

        if ( m_Plan.Reset )
            for ( auto& entry : m_Emitters )
                ResetInstance( entry.second );

        const double stepSeconds = m_Clock.GetSettings().StepSeconds;

        for ( auto& entry : m_Emitters )
            entry.second.Seen = false;

        auto view = registry.view<ECS::ParticleEmitterComponent, ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, ECS::ParticleEmitterComponent& emitter, const ECS::UUIDComponent& id )
             {
                 const std::uint64_t uuid  = id.UUID;
                 auto [it, created]        = m_Emitters.try_emplace( uuid );
                 EmitterInstance& instance = it->second;
                 if ( created )
                 {
                     instance.Seed = MakeEmitterSeed( kComponentSystemSeed, uuid, kComponentEmitterIndex );
                     ResetInstance( instance );
                 }
                 instance.Seen = true;
                 instance.Steps.clear();

                 // The editor's Restart: this instance alone starts over. Consumed before the enabled check
                 // so a disabled emitter also comes back empty.
                 if ( emitter.RequestRestart )
                 {
                     emitter.RequestRestart = false;
                     ResetInstance( instance );
                 }

                 const auto& d = emitter.Data;
                 if ( !d.Enabled || d.MaxParticles <= 0 )
                     return;

                 // The component is an emitter of one SpawnRate module that runs forever; a non-looping one has
                 // no loop duration to spawn over, so it bears nothing. A `.dfx` emitter's plan comes from
                 // CompileSpawnPlan over its lifecycle and EmitterUpdate group.
                 VFXSpawnPlan plan;
                 plan.Lifecycle.Loop = Assets::Serialization::VFXLoopBehavior::Infinite;
                 plan.Rate           = d.Looping ? std::max( static_cast<double>( d.SpawnRate ), 0.0 ) : 0.0;

                 const auto*     transform = registry.try_get<ECS::TransformComponent>( entity );
                 const glm::vec3 emitterCm =
                      transform ? glm::vec3( transform->GetTransform()[3] ) : glm::vec3( 0.0f );
                 PlanEmitterSteps( instance, plan, m_Plan.StepCount, stepSeconds, m_Channels, emitterCm );
             } );

        std::erase_if( m_Emitters, []( const auto& entry ) { return !entry.second.Seen; } );
        m_Channels.ClearEntries(); // a channel holds one frame of entries (UE: a data channel is cleared per tick)
    }
} // namespace Desert::VFX

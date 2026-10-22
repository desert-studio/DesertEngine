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
            {
                entry.second.AgeSeconds = 0.0;
                for ( EmitterInstance& emitter : entry.second.Emitters )
                    ResetInstance( emitter );
            }

        const double stepSeconds = m_Clock.GetSettings().StepSeconds;

        for ( auto& entry : m_Systems )
            entry.second.Seen = false;

        // THE SCALABILITY DECISION (UE FNiagaraScalabilityManager, per world and per EFFECT TYPE) from the views
        // reported since the last tick: the first pass gathers every placed system as a candidate, the pure
        // calculator (VFXScalability) decides all of them at once - the instance limits rank across the whole
        // world - and the second pass applies each verdict through its type's cull reaction and plans the births.
        // No view (headless) = nothing culled, the system as authored.
        std::vector<VFXViewReport> views;
        views.swap( m_PendingViews );
        m_CulledSystems = 0;

        constexpr std::size_t kNoCandidate = std::numeric_limits<std::size_t>::max();
        struct Placed
        {
            SystemInstance*                                 System;
            ECS::VFXComponent*                              Component;
            std::uint64_t                                   UUID;
            glm::vec3                                       EmitterCm;
            const Assets::Serialization::VFXEffectTypeData* Type;
            const std::string*                              TypeName;
            std::size_t                                     Candidate;
        };
        std::vector<Placed>                  placed;
        std::vector<VFXScalabilityCandidate> candidates;
        const double                         tickSeconds = static_cast<double>( m_Plan.StepCount ) * stepSeconds;

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
                     // A new system (first sight, renamed, reloaded): every emitter starts over from its plan,
                     // and so does its scalability state.
                     const std::size_t count = data->Emitters.size();
                     system.System           = data;
                     system.Plans.assign( count, {} );
                     system.Runs.assign( count, false );
                     system.Emitters.assign( count, {} );
                     system.Culled        = false;
                     system.Deactivated   = false;
                     system.Reason        = VFXCullReason::None;
                     system.AgeSeconds    = 0.0;
                     system.HiddenSeconds = 0.0;
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

                 // The editor's Restart: this system alone starts over - a Deactivated one becomes a candidate
                 // again.
                 if ( vfx.RequestRestart )
                 {
                     vfx.RequestRestart = false;
                     system.Deactivated = false;
                     system.AgeSeconds  = 0.0;
                     for ( EmitterInstance& emitter : system.Emitters )
                         ResetInstance( emitter );
                 }

                 const auto*     transform = registry.try_get<ECS::TransformComponent>( entity );
                 const glm::mat4 world = transform ? glm::mat4( transform->GetTransform() ) : glm::mat4( 1.0f );
                 const glm::vec3 emitterCm( world[3] );
                 // The world AABB of the system's fixed local bounds (Arvo: each axis of the box through the
                 // transform's linear part).
                 glm::vec3 minCm = emitterCm;
                 glm::vec3 maxCm = emitterCm;
                 for ( int axis = 0; axis < 3; ++axis )
                     for ( int col = 0; col < 3; ++col )
                     {
                         const float lo = world[col][axis] * data->Bounds.Min[col];
                         const float hi = world[col][axis] * data->Bounds.Max[col];
                         minCm[axis] += std::min( lo, hi );
                         maxCm[axis] += std::max( lo, hi );
                     }
                 // The system's effect type (ValidateVFXSystemData requires one); a missing type spawns nothing.
                 const std::string&                              typeName = data->EffectType.value();
                 const Assets::Serialization::VFXEffectTypeData* type     = EffectType( typeName );

                 std::size_t candidate = kNoCandidate;
                 if ( !system.Deactivated )
                 {
                     // The pool slots it holds while playing: the running emitters' Capacity (the world budget).
                     std::uint32_t poolParticles = 0;
                     if ( vfx.Data.AutoActivate )
                         for ( std::size_t k = 0; k < system.Runs.size(); ++k )
                             poolParticles += system.Runs[k] ? data->Emitters[k].Capacity : 0u;
                     candidate = candidates.size();
                     candidates.push_back( { .Type          = type,
                                             .System        = data.get(),
                                             .BoundsMinCm   = minCm,
                                             .BoundsMaxCm   = maxCm,
                                             .AgeSeconds    = system.AgeSeconds,
                                             .HiddenSeconds = system.HiddenSeconds,
                                             .PoolParticles = poolParticles } );
                 }
                 placed.push_back( { &system, &vfx, uuid, emitterCm, type, &typeName, candidate } );
             } );

        const std::vector<VFXScalabilityVerdict> verdicts =
             EvaluateVFXScalability( views, candidates, tickSeconds );
        using Reaction = Assets::Serialization::VFXCullReaction;
        for ( const Placed& p : placed )
        {
            SystemInstance&             system  = *p.System;
            const bool                  decided = p.Candidate != kNoCandidate;
            const VFXScalabilityVerdict verdict = decided ? verdicts[p.Candidate] : VFXScalabilityVerdict{};
            if ( decided )
            {
                system.Reason        = verdict.Reason;
                system.HiddenSeconds = verdict.HiddenSeconds;
            }
            const bool culled = !p.Type || system.Deactivated || verdict.Reason != VFXCullReason::None;
            if ( p.Type && culled != system.Culled )
            {
                const Reaction reaction = p.Type->CullReaction;
                const bool     immediate =
                     reaction == Reaction::DeactivateImmediate || reaction == Reaction::DeactivateImmediateResume;
                const bool resumes =
                     reaction == Reaction::DeactivateResume || reaction == Reaction::DeactivateImmediateResume;
                if ( culled )
                {
                    if ( immediate )
                        for ( EmitterInstance& emitter : system.Emitters )
                            ResetInstance( emitter ); // a new generation: the GPU zeroes its live particles
                    system.Deactivated = !resumes;
                    LOG_INFO( "[VFX] entity {} culled ({}): nearest view {:.0f} cm, significance {:.3g} - effect "
                              "type '{}', live particles {}, {}",
                              p.UUID, VFXCullReasonName( verdict.Reason ), verdict.NearestCm, verdict.Significance,
                              *p.TypeName, immediate ? "killed" : "finish",
                              resumes ? "resumes when back in scalability" : "stays off until Restart" );
                    if ( verdict.Reason == VFXCullReason::WorldBudget )
                        LOG_INFO( "[VFX] entity {} refused by the world particle budget: {} pool particles over the "
                                  "{} the more significant systems already hold of Effects.MaxWorldParticles {}",
                                  p.UUID, candidates[p.Candidate].PoolParticles, verdict.BudgetTakenBefore,
                                  verdict.BudgetParticles );
                }
                else
                    LOG_INFO( "[VFX] entity {} back in scalability, spawning resumes: nearest view {:.0f} cm "
                              "(effect type '{}')",
                              p.UUID, verdict.NearestCm, *p.TypeName );
            }
            system.Culled = culled;
            system.AgeSeconds += tickSeconds;
            m_CulledSystems += culled ? 1u : 0u;
            for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
            {
                EmitterInstance& emitter = system.Emitters[k];
                if ( !system.Runs[k] || !p.Component->Data.AutoActivate || culled )
                {
                    // Not active (UE bAutoActivate false), refused or culled: no births, the instance is kept
                    // (its live particles finish, or were reset above by an Immediate reaction).
                    emitter.Steps.clear();
                    emitter.ChannelSpawns.clear();
                    continue;
                }
                emitter.SpawnCountScale = verdict.SpawnCountScale;
                PlanEmitterSteps( emitter, system.Plans[k], m_Plan.StepCount, stepSeconds, m_Channels,
                                  p.EmitterCm );
            }
        }

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

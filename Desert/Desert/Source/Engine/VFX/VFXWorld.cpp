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
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>
#include <unordered_map>

namespace Desert::VFX
{
    void VFXWorld::ResetInstance( EmitterInstance& instance )
    {
        instance.Generation = ++m_LastGeneration;
        instance.NextId     = 0;
        instance.StepsRun   = 0;
        instance.FirstStep  = 0;
        instance.Spawn      = {};
    }

    namespace
    {
        // The pool slots a system holds while playing: its running emitters' Capacity (what the world budget
        // counts).
        std::uint32_t RunningPoolParticles( const SystemInstance& system )
        {
            std::uint32_t total = 0;
            if ( !system.System )
                return total;
            for ( std::size_t k = 0; k < system.Runs.size() && k < system.System->Emitters.size(); ++k )
                total += system.Runs[k] ? system.System->Emitters[k].Capacity : 0u;
            return total;
        }
    } // namespace

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
                e.Bounds   = data.Bounds;
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

        instance.FirstStep = instance.StepsRun;
        instance.StepsRun += stepCount;
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
        if ( m_DebugSettings.Frozen )
        {
            // The debugger's pause: no time passes but the steps it asked for. One StepSeconds after a remainder
            // below one step is exactly one step.
            seconds = m_FrozenSteps > 0 ? m_Clock.GetSettings().StepSeconds : 0.0;
            if ( m_FrozenSteps > 0 )
                --m_FrozenSteps;
        }
        if ( m_DesiredAge )
            m_Clock.SeekTo( *m_DesiredAge ); // served by this Advance: no steps when the age is already reached
        m_Plan = m_Clock.Advance( m_DesiredAge ? 0.0 : seconds );

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
                 // The editor's working copy, when the component carries one, is what plays (VFX-12).
                 std::shared_ptr<const Assets::Serialization::VFXSystemData> data = vfx.EditedSystem;
                 if ( !data )
                 {
                     const auto resolved = systems->Get( vfx.Data.System );
                     if ( !resolved.IsSuccess() || !resolved.GetValue() )
                         return;
                     data = resolved.GetValue();
                 }

                 const std::uint64_t uuid   = id.UUID;
                 SystemInstance&     system = m_Systems[uuid];
                 if ( system.System != data )
                 {
                     const bool firstSight = !system.System;
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
                     system.Overrides = vfx.Data.Overrides;
                     for ( std::size_t k = 0; k < count; ++k )
                     {
                         auto plan = CompileSpawnPlan( *data, k, system.Overrides );
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
                     // UE: a component registered (its World Partition cell streamed in, or placed) activates a
                     // fresh system - nothing survives an unload, the seed alone makes it the same effect again.
                     LOG_INFO( "[VFX] entity {} system {} ({} emitter(s), {} pool particles, seed {})", uuid,
                               firstSight ? "spawned" : "restarted (asset changed)", count,
                               RunningPoolParticles( system ), data->Seed );
                 }
                 else if ( system.Overrides != vfx.Data.Overrides )
                 {
                     // The placement's User values moved (Details, VFX.setVariable): the plans read them again;
                     // the instances keep their age and carried fractions.
                     system.Overrides = vfx.Data.Overrides;
                     for ( std::size_t k = 0; k < system.Plans.size(); ++k )
                     {
                         auto plan = CompileSpawnPlan( *data, k, system.Overrides );
                         if ( !plan.IsSuccess() )
                         {
                             LOG_ERROR( "[VFX] entity {} emitter {} '{}' bears nothing: {}", uuid, k,
                                        data->Emitters[k].Name, plan.GetError() );
                             system.Runs[k] = false;
                         }
                         else
                         {
                             system.Plans[k] = plan.GetValue();
                             system.Runs[k]  = data->Emitters[k].Enabled;
                         }
                     }
                 }
                 system.Seen   = true;
                 system.Entity = entity;

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
                 system.Transform = world;
                 // The world box of the enabled emitters' fixed bounds, placed as the renderer culls them
                 // (VFXBoundsAt: the placement's full transform); no enabled emitter - the placement's point.
                 const auto extent = Assets::Serialization::VFXSystemExtent( *data );
                 const Common::Math::AABB placed =
                      extent ? Assets::Serialization::VFXBoundsAt( *extent, world )
                             : Common::Math::AABB{ emitterCm, emitterCm };
                 const glm::vec3 minCm = placed.Min;
                 const glm::vec3 maxCm = placed.Max;
                 // The system's effect type (ValidateVFXSystemData requires one); a missing type spawns nothing.
                 const std::string&                              typeName = data->EffectType.value();
                 const Assets::Serialization::VFXEffectTypeData* type     = EffectType( typeName );

                 std::size_t candidate = kNoCandidate;
                 if ( !system.Deactivated )
                 {
                     // The pool slots it holds while playing: the running emitters' Capacity (the world budget).
                     const std::uint32_t poolParticles =
                          vfx.Data.AutoActivate ? RunningPoolParticles( system ) : 0u;
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
                        LOG_INFO(
                             "[VFX] entity {} refused by the world particle budget: {} pool particles over the "
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
            m_CulledSystems += culled ? 1u : 0u;

            // A LEVEL SEQUENCE OWNS THIS EFFECT'S AGE (UE: the Niagara system track sets DesiredAge on the
            // component; NiagaraComponent.cpp:995-1032 seeks it). The world's clock is not consulted: this
            // instance alone is reset when the age goes back (a new generation - the GPU zeroes it) and replays
            // its own fixed steps from its seed up to the age, under the seek budget per tick, so an age reached
            // by scrubbing is the state reached by playing. Outside every section it holds no particles.
            if ( const auto& drive = p.Component->SequencerDrive )
            {
                const std::uint32_t target =
                     drive->Active ? static_cast<std::uint32_t>( std::floor( drive->AgeSeconds / stepSeconds + 1e-6 ) )
                                   : 0u;
                const std::uint32_t budget = m_Clock.GetSettings().MaxSeekStepsPerTick;
                std::uint32_t       reached = 0;
                for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                {
                    EmitterInstance& emitter = system.Emitters[k];
                    if ( target < emitter.StepsRun || ( !drive->Active && ( emitter.StepsRun > 0 || emitter.NextId > 0 ) ) )
                        ResetInstance( emitter );
                    const std::uint32_t steps = std::min( target - emitter.StepsRun, budget );
                    if ( !drive->Active || !system.Runs[k] || culled || steps == 0 )
                    {
                        emitter.Steps.clear();
                        emitter.ChannelSpawns.clear();
                        if ( drive->Active && system.Runs[k] && !culled )
                            reached = std::max( reached, emitter.StepsRun );
                        continue;
                    }
                    emitter.SpawnCountScale = verdict.SpawnCountScale;
                    PlanEmitterSteps( emitter, system.Plans[k], steps, stepSeconds, m_Channels, p.EmitterCm );
                    reached = std::max( reached, emitter.StepsRun );
                }
                system.AgeSeconds = static_cast<double>( reached ) * stepSeconds;
                continue;
            }

            system.AgeSeconds += tickSeconds;
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

        // A system whose entity is gone (its World Partition cell unloaded, or destroyed) or whose slot emptied is
        // destroyed with it, as UE destroys a component's FNiagaraSystemInstance on unregister: no state is kept
        // for a later reload, it stops being a candidate (instance limits, world budget) from this tick, and the
        // renderer returns its pool ranges on its next tick (ParticleWorldGpu::PrepareTick).
        for ( auto it = m_Systems.begin(); it != m_Systems.end(); )
        {
            if ( it->second.Seen )
            {
                ++it;
                continue;
            }
            LOG_INFO( "[VFX] entity {} system released (entity gone or slot emptied): {} pool particles returned, "
                      "no longer counted in the instance limits or the world budget",
                      it->first, RunningPoolParticles( it->second ) );
            it = m_Systems.erase( it );
        }
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

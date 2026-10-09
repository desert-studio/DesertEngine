#include "VFXWorld.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXCurveLUT.hpp>
#include <Engine/VFX/VFXRandom.hpp>

#include <Common/Core/Logger.hpp>

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

        // The EmitterKey::Emitter of a report about a whole VFXComponent rather than one of its emitters.
        constexpr std::uint32_t kWholeComponent = 0xFFFFFFFFu;
    } // namespace

    bool VFXWorld::FirstReport( const EmitterKey& key )
    {
        return m_Reported.insert( key ).second;
    }

    EmitterInstance& VFXWorld::Visit( const EmitterKey& key, const std::uint32_t seed, const bool restart )
    {
        auto [it, created]        = m_Emitters.try_emplace( key );
        EmitterInstance& instance = it->second;
        if ( created )
        {
            instance.Seed = seed;
            ResetInstance( instance );
        }
        instance.Seen = true;
        instance.Steps.clear();
        if ( restart )
            ResetInstance( instance );
        return instance;
    }

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
        m_Reported.clear();
        // m_LastGeneration is NOT reset: a renderer may still hold GPU state stamped with an old
        // generation, and a fresh instance must never be mistaken for it.
    }

    const EmitterInstance* VFXWorld::FindEmitter( std::uint64_t entityUuid ) const
    {
        const auto it = m_Emitters.find( EmitterKey{ entityUuid, kComponentEmitterIndex, false } );
        return it != m_Emitters.end() ? &it->second : nullptr;
    }

    const EmitterInstance* VFXWorld::FindSystemEmitter( std::uint64_t entityUuid,
                                                        std::uint32_t emitterIndex ) const
    {
        const auto it = m_Emitters.find( EmitterKey{ entityUuid, emitterIndex, true } );
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
                 const std::uint64_t uuid = id.UUID;
                 // The editor's Restart: this instance alone starts over. Consumed before the enabled check
                 // so a disabled emitter also comes back empty.
                 const bool restart     = emitter.RequestRestart;
                 emitter.RequestRestart = false;
                 EmitterInstance& instance =
                      Visit( EmitterKey{ uuid, kComponentEmitterIndex, false },
                             MakeEmitterSeed( kComponentSystemSeed, uuid, kComponentEmitterIndex ), restart );

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

        // A VFXComponent plays its system's enabled emitters, instance (entity, emitter index) each. Not
        // activated = no instance (an existing one is dropped below as unseen). The emitters' GPU run is the
        // renderer's (ParticleWorldGpu); this plans their births exactly as a ParticleEmitterComponent's.
        auto systems = registry.view<ECS::VFXComponent, ECS::UUIDComponent>();
        systems.each(
             [&]( entt::entity entity, ECS::VFXComponent& vfx, const ECS::UUIDComponent& id )
             {
                 const std::uint64_t uuid    = id.UUID;
                 const bool          restart = vfx.RequestRestart;
                 vfx.RequestRestart          = false;
                 if ( !vfx.Data.AutoActivate )
                     return;

                 const EmitterKey whole{ uuid, kWholeComponent, true };
                 if ( !m_SystemLookup )
                 {
                     if ( FirstReport( whole ) )
                         LOG_ERROR( "[VFX] Entity {} plays a VFX system, but this scene's VFX world was given no "
                                    "system lookup; it spawns nothing",
                                    uuid );
                     return;
                 }
                 const Assets::Serialization::VFXSystemData* system = m_SystemLookup( vfx.Data.System );
                 if ( !system )
                 {
                     if ( FirstReport( whole ) )
                         LOG_ERROR( "[VFX] Entity {} plays VFX system {}, which is not loaded; it spawns nothing",
                                    uuid, static_cast<std::uint64_t>( vfx.Data.System ) );
                     return;
                 }

                 const auto*     transform = registry.try_get<ECS::TransformComponent>( entity );
                 const glm::vec3 emitterCm =
                      transform ? glm::vec3( transform->GetTransform()[3] ) : glm::vec3( 0.0f );
                 for ( std::size_t i = 0; i < system->Emitters.size(); ++i )
                 {
                     if ( !system->Emitters[i].Enabled )
                         continue;
                     const auto       index = static_cast<std::uint32_t>( i );
                     const EmitterKey key{ uuid, index, true };
                     auto             plan = CompileSpawnPlan( *system, i );
                     if ( !plan )
                     {
                         if ( FirstReport( key ) )
                             LOG_ERROR( "[VFX] Entity {}: emitter {} of its VFX system sits out: {}", uuid, index,
                                        plan.GetError() );
                         continue;
                     }
                     CompiledStack& compiled = m_Stacks[key];
                     if ( compiled.Source != system || restart || !compiled.Stack )
                     {
                         compiled   = {};
                         auto stack = CompileEmitterStack( *system, i, EngineModuleDir() );
                         if ( !stack )
                         {
                             if ( FirstReport( key ) )
                                 LOG_ERROR(
                                      "[VFX] Entity {}: emitter {} of its VFX system sits out, its stack does "
                                      "not compile: {}",
                                      uuid, index, stack.GetError() );
                             m_Stacks.erase( key );
                             continue;
                         }
                         auto atlas = BuildCurveAtlas( *system );
                         if ( !atlas )
                         {
                             if ( FirstReport( key ) )
                                 LOG_ERROR(
                                      "[VFX] Entity {}: emitter {} of its VFX system sits out, its curves do "
                                      "not bake: {}",
                                      uuid, index, atlas.GetError() );
                             m_Stacks.erase( key );
                             continue;
                         }
                         compiled.Source = system;
                         compiled.Stack =
                              std::make_shared<const VFXCompiledEmitter>( std::move( stack.GetValue() ) );
                         compiled.Curves = std::move( atlas.GetValue() );
                     }
                     // The rows carry this tick's input values: an edited value reaches the GPU without a compile.
                     auto params = BuildEmitterParams( *compiled.Stack, *system, i, compiled.Curves );
                     if ( !params )
                     {
                         if ( FirstReport( key ) )
                             LOG_ERROR(
                                  "[VFX] Entity {}: emitter {} of its VFX system sits out, its parameters do "
                                  "not build: {}",
                                  uuid, index, params.GetError() );
                         continue;
                     }
                     EmitterInstance& instance =
                          Visit( key, MakeEmitterSeed( system->Seed, uuid, index ), restart );
                     PlanEmitterSteps( instance, plan.GetValue(), m_Plan.StepCount, stepSeconds, m_Channels,
                                       emitterCm );
                     instance.Stack    = compiled.Stack;
                     instance.Params   = std::move( params.GetValue() );
                     instance.Curves   = compiled.Curves.Floats;
                     instance.Capacity = system->Emitters[i].Capacity;
                 }
             } );

        std::erase_if( m_Emitters, []( const auto& entry ) { return !entry.second.Seen; } );
        std::erase_if( m_Stacks, [this]( const auto& entry ) { return !m_Emitters.contains( entry.first ); } );
        m_Channels.ClearEntries(); // a channel holds one frame of entries (UE: a data channel is cleared per tick)
    }
} // namespace Desert::VFX

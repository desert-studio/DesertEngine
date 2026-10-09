#include "ParticleWorldGpu.hpp"

#include "ParticleEmitterRetire.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Desert::Graphic::System
{
    ParticleWorldGpu& ParticleWorldGpu::Of( const VFX::VFXWorld& world )
    {
        if ( auto* state = dynamic_cast<ParticleWorldGpu*>( world.GetGpuState() ) )
            return *state;
        auto  made  = std::make_unique<ParticleWorldGpu>();
        auto& state = *made;
        world.SetGpuState( std::move( made ) );
        return state;
    }

    ParticleEmitterGpu& ParticleWorldGpu::GetOrCreate( const uint32_t entityId, const uint32_t stepCapacity )
    {
        auto& e = m_Emitters[entityId];
        if ( !e.Counters )
            e.Counters = ShaderResources::StorageBuffer::Create( "ParticleCounters",
                                                                 kParticleDrawSlots * kParticleDrawSlotStride, 1 );
        if ( !e.DispatchArgs )
            e.DispatchArgs = ShaderResources::StorageBuffer::Create(
                 "ParticleDispatchArgs", kParticleDispatchArgsCount * kParticleDispatchArgsStride, 1 );
        if ( !e.ChannelSpawns )
        {
            e.ChannelCapacity = 1;
            e.ChannelSpawns =
                 ShaderResources::StorageBuffer::Create( "ParticleChannelSpawns", kParticleChannelSpawnStride, 1 );
        }
        if ( e.StepCapacity != stepCapacity || !e.Steps )
        {
            e.StepCapacity = stepCapacity;
            e.Steps        = ShaderResources::StorageBuffer::Create(
                 "ParticleSteps", std::max( 1u, stepCapacity ) * kParticleStepStride, 1 );
        }
        return e;
    }

    bool ParticleWorldGpu::EnsurePoolCapacity( const uint32_t particles )
    {
        if ( particles <= m_Pool.Capacity && m_Pool.Particles )
            return true;
        // Grown to at least double, so a scene adding emitters one by one recreates the pool a logarithmic number
        // of times. The contents are not carried over: every emitter restarts (compact 0 resets its range).
        const uint32_t capacity = std::max( particles, m_Pool.Capacity * 2u );
        m_Pool                  = {};
        m_Pool.Particles = ShaderResources::StorageBuffer::Create( "ParticlePool", capacity * kParticleStride, 1,
                                                                   /*persistent=*/true );
        m_Pool.FreeList  = ShaderResources::StorageBuffer::Create( "ParticleFreeList", capacity * 4u, 2, true );
        // Two alive lists per range (ping-pong halves): a compact reads the half the previous compact wrote.
        m_Pool.AliveList = ShaderResources::StorageBuffer::Create( "ParticleAliveList", capacity * 8u, 2, true );
        if ( !m_Pool.Particles || !m_Pool.FreeList || !m_Pool.AliveList )
        {
            LOG_ERROR( "[Particles] the scene's pool of {} particles could not be created: no emitter runs",
                       capacity );
            m_Pool = {};
            return false;
        }
        m_Pool.Capacity = capacity;
        for ( auto& [key, gpu] : m_Emitters )
            gpu.NeedsReset = true;
        return true;
    }

    bool ParticleWorldGpu::PrepareTick( const ::Desert::Core::Scene& scene )
    {
        const VFX::VFXWorld& world = scene.GetVFXWorld();
        if ( !m_Claim.Claim( world.GetTickSerial() ) )
            return false; // another view of this scene simulates this tick; this one draws the same pool

        m_FrameEmitters.clear();

        const auto&    clock        = world.GetClock().GetSettings();
        const auto     stepSeconds  = static_cast<float>( clock.StepSeconds );
        const uint32_t stepCapacity = std::max( clock.MaxStepsPerTick, clock.MaxSeekStepsPerTick );

        const auto& reg = scene.GetRegistry();

        const std::size_t retired = RetireDestroyedEmitters( m_Emitters, reg );
        if ( retired > 0 )
            LOG_INFO( "[Particles] Released {} emitter(s) whose entity is gone.", retired );
        m_Ranges.ReleaseUnless( [this]( const uint32_t key ) { return m_Emitters.contains( key ); } );

        auto view = reg.view<const ECS::ParticleEmitterComponent, const ECS::TransformComponent,
                             const ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, const ECS::ParticleEmitterComponent& emitter,
                  const ECS::TransformComponent& transform, const ECS::UUIDComponent& id )
             {
                 const auto& d = emitter.Data;
                 if ( !d.Enabled || d.MaxParticles <= 0 )
                     return;

                 const VFX::EmitterInstance* instance = world.FindEmitter( static_cast<uint64_t>( id.UUID ) );
                 if ( instance == nullptr )
                     return;

                 const auto          entityId = static_cast<uint32_t>( entity );
                 ParticleEmitterGpu& gpu      = GetOrCreate( entityId, stepCapacity );
                 if ( !gpu.Steps || !gpu.Counters || !gpu.DispatchArgs || !gpu.ChannelSpawns )
                     return;

                 // The emitter's range of the pool; a new or moved range starts dead.
                 const ParticlePoolRange range =
                      m_Ranges.Acquire( entityId, static_cast<uint32_t>( d.MaxParticles ) );
                 if ( range.Base != gpu.Range.Base || range.Count != gpu.Range.Count )
                     gpu.NeedsReset = true;
                 gpu.Range = range;
                 // A state whose VFXWorld generation was moved past (seek, restart) restarts.
                 if ( gpu.Generation != instance->Generation )
                 {
                     gpu.NeedsReset = true;
                     gpu.Generation = instance->Generation;
                 }

                 uint32_t stepCount =
                      std::min( static_cast<uint32_t>( instance->Steps.size() ), gpu.StepCapacity );
                 if ( stepCount > 0 )
                 {
                     std::vector<ParticleStepGpu> table( stepCount );
                     for ( uint32_t s = 0; s < stepCount; ++s )
                         table[s] = { instance->Steps[s].IdBase, instance->Seed, instance->Steps[s].Budget,
                                      instance->Steps[s].ChannelFirst, instance->Steps[s].ChannelCount };
                     const auto uploaded = gpu.Steps->SetData(
                          table.data(), stepCount * static_cast<uint32_t>( sizeof( ParticleStepGpu ) ) );
                     if ( !uploaded.IsSuccess() )
                     {
                         LOG_ERROR( "[Particles] emitter {} does not simulate this tick, its step table did "
                                    "not upload: {}",
                                    entityId, uploaded.GetError() );
                         stepCount = 0;
                     }
                 }

                 // This tick's Spawn from Channel particles, one record per particle (VFX-10): step s's spawn t <
                 // ChannelCount reads record ChannelFirst + t. Grown x2 when a tick brings more.
                 std::vector<ParticleChannelSpawnGpu> channel;
                 for ( const VFX::VFXChannelSpawnRequest& r : instance->ChannelSpawns )
                     for ( uint32_t k = 0; k < r.Count; ++k )
                         channel.push_back(
                              { glm::vec4( r.Position, r.HasPosition ? 1.0f : 0.0f ),
                                glm::vec4( r.Direction, r.HasDirection ? 1.0f : 0.0f ),
                                r.HasColor ? r.Color : glm::vec4( 1.0f ),
                                glm::vec4( r.Lifetime, r.HasSize ? r.Size : 1.0f, r.HasColor ? 1.0f : 0.0f,
                                           r.HasLifetime ? 1.0f : 0.0f ) } );
                 if ( stepCount > 0 && !channel.empty() )
                 {
                     const auto needed = static_cast<uint32_t>( channel.size() );
                     if ( needed > gpu.ChannelCapacity )
                     {
                         gpu.ChannelCapacity = std::max( needed, gpu.ChannelCapacity * 2u );
                         gpu.ChannelSpawns   = ShaderResources::StorageBuffer::Create(
                              "ParticleChannelSpawns", gpu.ChannelCapacity * kParticleChannelSpawnStride, 1 );
                     }
                     if ( !gpu.ChannelSpawns )
                     {
                         LOG_ERROR(
                              "[Particles] emitter {} sits out this tick, the buffer of its {} channel spawns "
                              "was not created",
                              entityId, needed );
                         return;
                     }
                     const auto uploaded =
                          gpu.ChannelSpawns->SetData( channel.data(), needed * kParticleChannelSpawnStride );
                     if ( !uploaded.IsSuccess() )
                     {
                         LOG_ERROR(
                              "[Particles] emitter {} does not simulate this tick, its {} channel spawns did "
                              "not upload: {}",
                              entityId, needed, uploaded.GetError() );
                         stepCount = 0;
                     }
                 }
                 if ( !gpu.ChannelSpawns )
                     return;

                 // Both draw slots start empty: compact 0 fills slot 0 from the pool, so the counters need no
                 // history (ParticleCompact). Slot h draws alive half h: six vertices per entry from 6 x its
                 // start.
                 ParticleDrawSlotGpu slots[kParticleDrawSlots];
                 for ( uint32_t h = 0; h < kParticleDrawSlots; ++h )
                     slots[h].FirstVertex = ( 2u * range.Base + h * range.Count ) * 6u;
                 const auto counted = gpu.Counters->SetData( slots, static_cast<uint32_t>( sizeof( slots ) ) );
                 if ( !counted.IsSuccess() )
                 {
                     LOG_ERROR( "[Particles] emitter {} sits out this tick, its counters did not upload: {}",
                                entityId, counted.GetError() );
                     return;
                 }

                 const glm::vec3 worldPos = glm::vec3( transform.GetTransform()[3] );
                 glm::vec3       dir      = d.Direction;
                 if ( glm::dot( dir, dir ) < 1e-6f )
                     dir = glm::vec3( 0.0f, 1.0f, 0.0f );
                 dir = glm::normalize( dir );

                 ParticleFrameEmitter fe;
                 fe.EntityId        = entityId;
                 fe.Gpu             = &gpu;
                 fe.Material        = d.Material;
                 fe.StepCount       = stepCount;
                 fe.Push.EmitterPos = glm::vec4( worldPos, stepSeconds );
                 fe.Push.Gravity    = glm::vec4( d.Gravity, 0.0f );
                 fe.Push.Direction  = glm::vec4( dir, glm::radians( d.ConeAngle ) );
                 fe.Push.Params     = glm::vec4( d.StartSpeed, d.SpeedVariance, d.Lifetime, d.LifetimeVariance );
                 fe.Push.StartColor = glm::vec4( d.StartColor, d.StartAlpha );
                 fe.Push.EndColor   = glm::vec4( d.EndColor, d.EndAlpha );
                 fe.Push.Sizes      = glm::vec4( d.StartSize, d.EndSize, d.SizeCurvePower, 0.0f );
                 fe.Push.Counts     = glm::uvec4( range.Count, 0u, range.Base, d.WorldSpace ? 0u : 1u );

                 m_FrameEmitters.push_back( fe );
             } );

        if ( !m_FrameEmitters.empty() && !EnsurePoolCapacity( m_Ranges.End() ) )
            m_FrameEmitters.clear();
        return true;
    }
} // namespace Desert::Graphic::System

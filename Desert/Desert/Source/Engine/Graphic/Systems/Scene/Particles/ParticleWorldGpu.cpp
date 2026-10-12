#include "ParticleWorldGpu.hpp"

#include "ParticleEmitterRetire.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXSimulationProgram.hpp>
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

    ParticleEmitterGpu& ParticleWorldGpu::GetOrCreate( const uint64_t key, const uint32_t stepCapacity )
    {
        auto& e = m_Emitters[key];
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

    bool ParticleWorldGpu::EnsurePoolCapacity( const uint32_t particles, const uint32_t floats,
                                               const uint32_t ints )
    {
        if ( m_Pool.Particles && particles <= m_Pool.Capacity && floats <= m_Pool.FloatCapacity &&
             ints <= m_Pool.IntCapacity )
            return true;
        // Grown to at least double, so a scene adding emitters one by one recreates the pool a logarithmic number
        // of times. The contents are not carried over: every emitter restarts (compact 0 resets its range).
        const auto grown = []( const uint32_t needed, const uint32_t had )
        { return std::max( { 1u, needed, needed > had ? had * 2u : had } ); };
        const uint32_t capacity      = grown( particles, m_Pool.Capacity );
        const uint32_t floatCapacity = grown( floats, m_Pool.FloatCapacity );
        const uint32_t intCapacity   = grown( ints, m_Pool.IntCapacity );
        m_Pool                       = {};
        m_Pool.Particles = ShaderResources::StorageBuffer::Create( "ParticlePool", capacity * kParticleStride, 1,
                                                                   /*persistent=*/true );
        m_Pool.Slots     = ShaderResources::StorageBuffer::Create(
             "ParticleSlots", capacity * static_cast<uint32_t>( sizeof( VFXSlotStateGpu ) ), 1, true );
        m_Pool.Floats    = ShaderResources::StorageBuffer::Create( "ParticleFloats", floatCapacity * 4u, 1, true );
        m_Pool.Ints      = ShaderResources::StorageBuffer::Create( "ParticleInts", intCapacity * 4u, 1, true );
        m_Pool.FreeList  = ShaderResources::StorageBuffer::Create( "ParticleFreeList", capacity * 4u, 2, true );
        // Two alive lists per range (ping-pong halves): a compact reads the half the previous compact wrote.
        m_Pool.AliveList = ShaderResources::StorageBuffer::Create( "ParticleAliveList", capacity * 8u, 2, true );
        if ( !m_Pool.Particles || !m_Pool.Slots || !m_Pool.Floats || !m_Pool.Ints || !m_Pool.FreeList ||
             !m_Pool.AliveList )
        {
            LOG_ERROR( "[Particles] the scene's pool of {} particles ({} float, {} int components) could not be "
                       "created: no emitter runs",
                       capacity, floatCapacity, intCapacity );
            m_Pool = {};
            return false;
        }
        m_Pool.Capacity      = capacity;
        m_Pool.FloatCapacity = floatCapacity;
        m_Pool.IntCapacity   = intCapacity;
        for ( auto& [key, gpu] : m_Emitters )
            gpu.NeedsReset = true;
        return true;
    }

    ParticleEmitterGpu* ParticleWorldGpu::PrepareEmitter( const uint64_t key, const VFX::EmitterInstance& instance,
                                                          const uint32_t capacity, const uint32_t stepCapacity,
                                                          uint32_t& stepCount )
    {
        const uint32_t      entityId = ParticleEmitterKey::Entity( key );
        const uint32_t      emitter  = ParticleEmitterKey::Emitter( key );
        ParticleEmitterGpu& gpu      = GetOrCreate( key, stepCapacity );
        if ( !gpu.Steps || !gpu.Counters || !gpu.DispatchArgs || !gpu.ChannelSpawns )
            return nullptr;

        // The emitter's range of the pool; a new or moved range starts dead.
        const ParticlePoolRange range = m_Ranges.Acquire( key, capacity );
        if ( range.Base != gpu.Range.Base || range.Count != gpu.Range.Count )
            gpu.NeedsReset = true;
        gpu.Range = range;
        // A state whose VFXWorld generation was moved past (seek, restart) restarts.
        if ( gpu.Generation != instance.Generation )
        {
            gpu.NeedsReset = true;
            gpu.Generation = instance.Generation;
        }

        stepCount = std::min( static_cast<uint32_t>( instance.Steps.size() ), gpu.StepCapacity );
        if ( stepCount > 0 )
        {
            std::vector<ParticleStepGpu> table( stepCount );
            for ( uint32_t s = 0; s < stepCount; ++s )
                table[s] = { instance.Steps[s].IdBase, instance.Seed, instance.Steps[s].Budget,
                             instance.Steps[s].ChannelFirst, instance.Steps[s].ChannelCount };
            const auto uploaded = gpu.Steps->SetData(
                 table.data(), stepCount * static_cast<uint32_t>( sizeof( ParticleStepGpu ) ) );
            if ( !uploaded.IsSuccess() )
            {
                LOG_ERROR(
                     "[Particles] emitter {} of entity {} does not simulate this tick, its step table did not "
                     "upload: {}",
                     emitter, entityId, uploaded.GetError() );
                stepCount = 0;
            }
        }

        // This tick's Spawn from Channel particles, one record per particle (VFX-10): step s's spawn t <
        // ChannelCount reads record ChannelFirst + t. Grown x2 when a tick brings more. Only ParticleSimulate
        // binds them; a compiled stack spawns from a channel through its own module.
        std::vector<ParticleChannelSpawnGpu> channel;
        if ( !instance.Stack )
            for ( const VFX::VFXChannelSpawnRequest& r : instance.ChannelSpawns )
                for ( uint32_t k = 0; k < r.Count; ++k )
                    channel.push_back( { glm::vec4( r.Position, r.HasPosition ? 1.0f : 0.0f ),
                                         glm::vec4( r.Direction, r.HasDirection ? 1.0f : 0.0f ),
                                         r.HasColor ? r.Color : glm::vec4( 1.0f ),
                                         glm::vec4( r.Lifetime, r.HasSize ? r.Size : 1.0f,
                                                    r.HasColor ? 1.0f : 0.0f, r.HasLifetime ? 1.0f : 0.0f ) } );
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
                LOG_ERROR( "[Particles] emitter {} of entity {} sits out this tick, the buffer of its {} channel "
                           "spawns was not created",
                           emitter, entityId, needed );
                return nullptr;
            }
            const auto uploaded =
                 gpu.ChannelSpawns->SetData( channel.data(), needed * kParticleChannelSpawnStride );
            if ( !uploaded.IsSuccess() )
            {
                LOG_ERROR(
                     "[Particles] emitter {} of entity {} does not simulate this tick, its {} channel spawns "
                     "did not upload: {}",
                     emitter, entityId, needed, uploaded.GetError() );
                stepCount = 0;
            }
        }
        if ( !gpu.ChannelSpawns )
            return nullptr;

        // Both draw slots start empty: compact 0 fills slot 0 from the pool, so the counters need no history
        // (ParticleCompact). Slot h draws alive half h: six vertices per entry from 6 x its start.
        ParticleDrawSlotGpu slots[kParticleDrawSlots];
        for ( uint32_t h = 0; h < kParticleDrawSlots; ++h )
            slots[h].FirstVertex = ( 2u * range.Base + h * range.Count ) * 6u;
        const auto counted = gpu.Counters->SetData( slots, static_cast<uint32_t>( sizeof( slots ) ) );
        if ( !counted.IsSuccess() )
        {
            LOG_ERROR( "[Particles] emitter {} of entity {} sits out this tick, its counters did not upload: {}",
                       emitter, entityId, counted.GetError() );
            return nullptr;
        }
        return &gpu;
    }

    namespace
    {
        // Uploads @p count elements of @p stride bytes to @p buffer, (re)made x2 when it holds fewer; false when
        // it failed (said in the log, naming the emitter).
        bool UploadGrown( std::shared_ptr<ShaderResources::StorageBuffer>& buffer, uint32_t& capacity,
                          const void* data, const uint32_t count, const uint32_t stride, const char* name,
                          const uint64_t key )
        {
            const uint32_t needed = std::max( 1u, count );
            if ( needed > capacity || !buffer )
            {
                capacity = std::max( needed, capacity * 2u );
                buffer   = ShaderResources::StorageBuffer::Create( name, capacity * stride, 1 );
            }
            if ( !buffer )
            {
                LOG_ERROR( "[Particles] emitter {} of entity {} sits out this tick, its {} buffer was not created",
                           ParticleEmitterKey::Emitter( key ), ParticleEmitterKey::Entity( key ), name );
                return false;
            }
            if ( count == 0 )
                return true;
            const auto uploaded = buffer->SetData( data, count * stride );
            if ( !uploaded.IsSuccess() )
                LOG_ERROR( "[Particles] emitter {} of entity {} sits out this tick, its {} did not upload: {}",
                           ParticleEmitterKey::Emitter( key ), ParticleEmitterKey::Entity( key ), name,
                           uploaded.GetError() );
            return uploaded.IsSuccess();
        }
    } // namespace

    bool ParticleWorldGpu::PrepareStack( const uint64_t key, ParticleEmitterGpu& gpu,
                                         const VFX::EmitterInstance& instance )
    {
        const VFX::VFXPoolRange components = VFX::PoolRangeOf( instance.Stack->Layout, gpu.Range.Count );
        if ( components.Floats > UINT32_MAX || components.Ints > UINT32_MAX )
        {
            LOG_ERROR(
                 "[Particles] emitter {} of entity {} sits out: {} particles of its layout take {} float and "
                 "{} int components, more than a pool addresses",
                 ParticleEmitterKey::Emitter( key ), ParticleEmitterKey::Entity( key ), gpu.Range.Count,
                 components.Floats, components.Ints );
            return false;
        }
        const ParticlePoolRange floats = m_FloatRanges.Acquire( key, static_cast<uint32_t>( components.Floats ) );
        const ParticlePoolRange ints   = m_IntRanges.Acquire( key, static_cast<uint32_t>( components.Ints ) );
        if ( floats.Base != gpu.FloatRange.Base || floats.Count != gpu.FloatRange.Count ||
             ints.Base != gpu.IntRange.Base || ints.Count != gpu.IntRange.Count )
            gpu.NeedsReset = true;
        gpu.FloatRange = floats;
        gpu.IntRange   = ints;

        // Each emitter's rows and curves are a buffer of its own, so its Bases.z / Bases.w are 0.
        return UploadGrown( gpu.Params, gpu.ParamsCapacity, instance.Params.data(),
                            static_cast<uint32_t>( instance.Params.size() ), 16u, "VFXParams", key ) &&
               UploadGrown( gpu.Curves, gpu.CurvesCapacity, instance.Curves.data(),
                            static_cast<uint32_t>( instance.Curves.size() ), 4u, "VFXCurves", key );
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
        const auto live = [this]( const uint64_t key ) { return m_Emitters.contains( key ); };
        m_Ranges.ReleaseUnless( live );
        m_FloatRanges.ReleaseUnless( live );
        m_IntRanges.ReleaseUnless( live );

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

                 const auto          entityId  = static_cast<uint32_t>( entity );
                 const uint64_t      key       = ParticleEmitterKey::Make( entityId, 0u, false );
                 uint32_t            stepCount = 0;
                 ParticleEmitterGpu* gpu = PrepareEmitter( key, *instance, static_cast<uint32_t>( d.MaxParticles ),
                                                           stepCapacity, stepCount );
                 if ( gpu == nullptr )
                     return;

                 const glm::vec3 worldPos = glm::vec3( transform.GetTransform()[3] );
                 glm::vec3       dir      = d.Direction;
                 if ( glm::dot( dir, dir ) < 1e-6f )
                     dir = glm::vec3( 0.0f, 1.0f, 0.0f );
                 dir = glm::normalize( dir );

                 const ParticlePoolRange range = gpu->Range;
                 ParticleFrameEmitter    fe;
                 fe.EntityId        = entityId;
                 fe.Key             = key;
                 fe.Gpu             = gpu;
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

        // A VFXComponent plays its system's planned emitters, (entity, emitter index) each, through the compiled
        // stacks' programs (VFX-03): the world compiled each stack and built its rows this tick.
        std::unordered_map<uint64_t, entt::entity> systemEntities;
        reg.view<const ECS::VFXComponent, const ECS::UUIDComponent>().each(
             [&]( entt::entity entity, const ECS::VFXComponent&, const ECS::UUIDComponent& id )
             { systemEntities.emplace( static_cast<uint64_t>( id.UUID ), entity ); } );
        for ( const auto& [emitterKey, instance] : world.GetEmitters() )
        {
            if ( !emitterKey.System || !instance.Stack || instance.Capacity == 0 )
                continue;
            const auto owner = systemEntities.find( emitterKey.Uuid );
            if ( owner == systemEntities.end() )
                continue;

            const auto          entityId  = static_cast<uint32_t>( owner->second );
            const uint64_t      key       = ParticleEmitterKey::Make( entityId, emitterKey.Emitter, true );
            uint32_t            stepCount = 0;
            ParticleEmitterGpu* gpu = PrepareEmitter( key, instance, instance.Capacity, stepCapacity, stepCount );
            if ( gpu == nullptr || !PrepareStack( key, *gpu, instance ) )
                continue;

            // The age the first of this tick's steps starts at; step s adds s steps (ParticleRenderer::Simulate).
            const double firstAge =
                 instance.Spawn.Age - static_cast<double>( instance.Steps.size() ) * clock.StepSeconds;

            ParticleFrameEmitter fe;
            fe.EntityId  = entityId;
            fe.Key       = key;
            fe.Gpu       = gpu;
            fe.Stack     = instance.Stack;
            fe.StepCount = stepCount;
            fe.StackPush.Time =
                 glm::vec4( stepSeconds, static_cast<float>( std::max( 0.0, firstAge ) ), 0.0f, 0.0f );
            fe.StackPush.Counts = glm::uvec4( gpu->Range.Count, 0u, gpu->Range.Base, 0u );
            fe.StackPush.Bases  = glm::uvec4( gpu->FloatRange.Base, gpu->IntRange.Base, 0u, 0u );
            m_FrameEmitters.push_back( fe );
        }

        if ( !m_FrameEmitters.empty() &&
             !EnsurePoolCapacity( m_Ranges.End(), m_FloatRanges.End(), m_IntRanges.End() ) )
            m_FrameEmitters.clear();
        return true;
    }
} // namespace Desert::Graphic::System

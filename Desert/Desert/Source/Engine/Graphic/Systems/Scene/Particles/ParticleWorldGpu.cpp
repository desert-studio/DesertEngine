#include "ParticleWorldGpu.hpp"

#include "ParticleEmitterRetire.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXWorld.hpp>
#include <Engine/VFX/VFXCurveLUT.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>

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

    namespace
    {
        // The attributes the host draws from, each with the one type it reads it as (Generated/VFXEmitterStack.glslh).
        struct DrawnAttribute
        {
            std::string_view Name;
            std::string_view Define;
            VFX::VFXValueType Type;
        };
        constexpr DrawnAttribute kDrawnAttributes[] = {
             { "Position", "VFX_ATTR_POSITION", VFX::VFXValueType::Vec3 },
             { "SpriteSize", "VFX_ATTR_SPRITESIZE", VFX::VFXValueType::Vec2 },
             { "Color", "VFX_ATTR_COLOR", VFX::VFXValueType::Vec4 },
             { "Age", "VFX_ATTR_AGE", VFX::VFXValueType::Float },
             { "Lifetime", "VFX_ATTR_LIFETIME", VFX::VFXValueType::Float },
             { "Velocity", "VFX_ATTR_VELOCITY", VFX::VFXValueType::Vec3 },
        };

        // The bytes Generated/VFXEmitterStack.glslh becomes for one compiled stack: its Particle block, then where
        // the host finds each attribute it draws from.
        Common::ResultStr<std::string> StackHostSource( const VFX::VFXCompiledEmitter& compiled )
        {
            const auto parsed = Core::Preprocess::DShaderParser::Parse( compiled.ShaderText );
            if ( !parsed.IsSuccess() )
                return Common::MakeError<std::string>( parsed.GetError() );
            std::string source = parsed.GetValue().Meta.ParticleSource;
            source += '\n';
            for ( const DrawnAttribute& drawn : kDrawnAttributes )
                if ( const auto* a = compiled.Layout.Find( drawn.Name ); a != nullptr && a->Type == drawn.Type )
                    source += std::format( "#define {} {}u\n", drawn.Define, a->FloatStart );
            return Common::MakeSuccess( std::move( source ) );
        }

        std::shared_ptr<ShaderResources::StorageBuffer> Columns( const char* name, uint32_t capacity,
                                                                 uint32_t columns, uint32_t binding )
        {
            return ShaderResources::StorageBuffer::Create( name, std::max( 1u, capacity * columns ) * 4u, binding,
                                                           /*persistent=*/true );
        }
    } // namespace

    bool ParticleWorldGpu::EnsureTickBuffers( ParticleEmitterGpu& e, const uint32_t stepCapacity )
    {
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
        return e.Steps && e.Counters && e.DispatchArgs && e.ChannelSpawns && e.AttributeFloats &&
               e.AttributeInts && e.Params && e.Curves;
    }

    std::shared_ptr<ComputePipeline> ParticleWorldGpu::ProgramFor( const uint64_t key, const std::string& name,
                                                                    const std::string& hostSource )
    {
        if ( const auto found = m_Programs.find( key ); found != m_Programs.end() )
            return found->second;
        std::shared_ptr<ComputePipeline>& program = m_Programs[key];
        auto* shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( shaders == nullptr )
            return nullptr;
        Core::ShaderVariant variant;
        variant.VirtualSources.push_back( { "Generated/VFXEmitterStack.glslh", hostSource } );
        const auto shader = shaders->AcquireVariant( "ParticleSimulate", variant );
        if ( !shader )
        {
            LOG_ERROR( "[VFX] stack {} has no host program: ParticleSimulate is not registered", name );
            return nullptr;
        }
        const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
        if ( !made )
        {
            LOG_ERROR( "[VFX] stack {} does not simulate: {}", name, made.GetError() );
            return nullptr;
        }
        program = made.GetValue();
        return program;
    }

    void ParticleWorldGpu::BuildEmitter( ParticleEmitterGpu& gpu, const Assets::Serialization::VFXSystemData& system,
                                         const std::size_t index, const uint32_t entityId )
    {
        const auto& emitter = system.Emitters[index];
        gpu.Capacity        = emitter.Enabled ? emitter.Capacity : 0u;
        gpu.Local           = emitter.Space == Assets::Serialization::VFXSimulationSpace::Local;
        if ( gpu.Capacity == 0 )
            return;
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[VFX] entity {} emitter {} '{}' does not simulate: {}", entityId, index, emitter.Name, why );
            gpu.Capacity = 0;
            gpu.Pipeline = nullptr;
        };

        const auto compiled = VFX::CompileEmitterStack( system, index, VFX::EngineModuleDir() );
        if ( !compiled.IsSuccess() )
            return refuse( compiled.GetError() );
        const auto curves = VFX::BuildCurveAtlas( system );
        if ( !curves.IsSuccess() )
            return refuse( curves.GetError() );
        auto params = VFX::BuildEmitterParams( compiled.GetValue(), system, index, curves.GetValue() );
        if ( !params.IsSuccess() )
            return refuse( params.GetError() );
        const auto source = StackHostSource( compiled.GetValue() );
        if ( !source.IsSuccess() )
            return refuse( source.GetError() );

        gpu.Pipeline = ProgramFor( compiled.GetValue().Key, compiled.GetValue().ShaderName, source.GetValue() );
        if ( !gpu.Pipeline )
            return refuse( "its host program did not build (logged above)" );
        gpu.ParamRows    = std::move( params.GetValue() );
        gpu.CurveFloats  = curves.GetValue().Floats;
        gpu.FloatColumns = compiled.GetValue().Layout.TotalFloatComponents;
        gpu.IntColumns   = compiled.GetValue().Layout.TotalIntComponents;
        if ( gpu.ParamRows.empty() )
            gpu.ParamRows.emplace_back( 0.0f );
        if ( gpu.CurveFloats.empty() )
            gpu.CurveFloats.push_back( 0.0f );
        gpu.AttributeFloats = Columns( "VFXAttributeFloats", gpu.Capacity, gpu.FloatColumns, 6 );
        gpu.AttributeInts   = Columns( "VFXAttributeInts", gpu.Capacity, gpu.IntColumns, 7 );
        gpu.Params          = ShaderResources::StorageBuffer::Create(
             "VFXStackParams", static_cast<uint32_t>( gpu.ParamRows.size() * sizeof( glm::vec4 ) ), 8 );
        gpu.Curves = ShaderResources::StorageBuffer::Create(
             "VFXStackCurves", static_cast<uint32_t>( gpu.CurveFloats.size() * sizeof( float ) ), 9 );
        gpu.NeedsReset = true;
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
        for ( auto& [key, system] : m_Systems )
            for ( ParticleEmitterGpu& gpu : system.Emitters )
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

        const std::size_t retired = RetireDestroyedEmitters( m_Systems, reg );
        if ( retired > 0 )
            LOG_INFO( "[Particles] Released {} system(s) whose entity is gone.", retired );
        m_Ranges.ReleaseUnless( [this]( const uint32_t key ) { return m_Systems.contains( key ); } );
        // The random streams' step: unique per fixed step of the world's life (u_Stack.x + step).
        const auto stepSerial = static_cast<uint32_t>( world.GetTickSerial() * std::max( 1u, stepCapacity ) );

        auto view = reg.view<const ECS::VFXComponent, const ECS::TransformComponent, const ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, const ECS::VFXComponent&, const ECS::TransformComponent& transform,
                  const ECS::UUIDComponent& id )
             {
                 const VFX::SystemInstance* instances = world.FindSystem( static_cast<uint64_t>( id.UUID ) );
                 if ( instances == nullptr || !instances->System )
                     return; // no system, or not read yet

                 const auto         entityId = static_cast<uint32_t>( entity );
                 ParticleSystemGpu& system   = m_Systems[entityId];
                 if ( system.System != instances->System )
                 {
                     // A new system for this entity: every emitter is compiled again (programs are shared by key).
                     system.System = instances->System;
                     system.Emitters.clear();
                     system.Emitters.resize( instances->System->Emitters.size() );
                     for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                         BuildEmitter( system.Emitters[k], *instances->System, k, entityId );
                 }

                 // One range of the pool per system, each emitter a slice of it in emitter order.
                 uint32_t total = 0;
                 for ( const ParticleEmitterGpu& gpu : system.Emitters )
                     total += gpu.Capacity;
                 if ( total == 0 )
                     return;
                 const ParticlePoolRange systemRange = m_Ranges.Acquire( entityId, total );
                 const glm::vec3         worldPos    = glm::vec3( transform.GetTransform()[3] );

                 uint32_t offset = 0;
                 for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                 {
                     ParticleEmitterGpu& gpu = system.Emitters[k];
                     if ( gpu.Capacity == 0 || k >= instances->Emitters.size() )
                         continue;
                     const VFX::EmitterInstance* instance = &instances->Emitters[k];
                     const ParticlePoolRange     range{ systemRange.Base + offset, gpu.Capacity };
                     offset += gpu.Capacity;
                     if ( !EnsureTickBuffers( gpu, stepCapacity ) )
                         continue;
                     if ( !PrepareEmitter( gpu, *instance, range, entityId, worldPos, stepSeconds, stepSerial ) )
                         continue;
                 }
             } );

        if ( !m_FrameEmitters.empty() && !EnsurePoolCapacity( m_Ranges.End() ) )
            m_FrameEmitters.clear();
        return true;
    }

    bool ParticleWorldGpu::PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                                           const ParticlePoolRange& range, const uint32_t entityId,
                                           const glm::vec3& worldPos, const float stepSeconds,
                                           const uint32_t stepSerial )
    {
        const VFX::EmitterInstance* instance = &emitter;
                 // The emitter's range of the pool; a new or moved range starts dead.
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
                     const auto params = gpu.Params->SetData(
                          gpu.ParamRows.data(), static_cast<uint32_t>( gpu.ParamRows.size() * sizeof( glm::vec4 ) ) );
                     const auto curves = gpu.Curves->SetData(
                          gpu.CurveFloats.data(), static_cast<uint32_t>( gpu.CurveFloats.size() * sizeof( float ) ) );
                     if ( !uploaded.IsSuccess() || !params.IsSuccess() || !curves.IsSuccess() )
                     {
                         LOG_ERROR( "[Particles] emitter {} does not simulate this tick, its step table, parameters "
                                    "or curves did not upload: {}",
                                    entityId,
                                    !uploaded.IsSuccess() ? uploaded.GetError()
                                                          : ( !params.IsSuccess() ? params.GetError()
                                                                                  : curves.GetError() ) );
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
                         return false;
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
                     return false;

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
                     return false;
                 }

                 ParticleFrameEmitter fe;
                 fe.EntityId     = entityId;
                 fe.Gpu          = &gpu;
                 fe.Pipeline     = gpu.Pipeline.get();
                 fe.StepCount    = stepCount;
                 fe.Push.EmitterPos = glm::vec4( worldPos, stepSeconds );
                 fe.Push.Counts     = glm::uvec4( range.Count, 0u, range.Base, gpu.Local ? 1u : 0u );
                 fe.Push.Stack      = glm::uvec4( stepSerial, 0u, 0u, 0u );
                 m_FrameEmitters.push_back( fe );
                 return true;
    }

} // namespace Desert::Graphic::System

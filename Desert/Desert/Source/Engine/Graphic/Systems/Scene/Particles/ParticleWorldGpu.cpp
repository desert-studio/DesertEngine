#include "ParticleWorldGpu.hpp"

#include "ParticleEmitterRetire.hpp"

#include <Engine/Assets/VFXModuleAsset.hpp>
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
#include <bit>
#include <cstdint>
#include <utility>
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
        // THE ONE TABLE of the attributes a renderer reads (UE: the sprite renderer's attribute bindings), each
        // with the one type it reads it as and its lane in ParticleDrawLayoutGpu (lane l = Columns{l / 4}[l % 4];
        // an Int attribute's lane holds its first INT column, the others their first float column). The
        // sprite vertex path and the sort read them out of the emitter's columns through
        // Common/ParticleDrawRead.glslh.
        struct DrawnAttribute
        {
            std::string_view  Name;
            VFX::VFXValueType Type;
            uint32_t          Lane;
        };
        constexpr DrawnAttribute kDrawnAttributes[] = {
             { "Position", VFX::VFXValueType::Vec3, 0 },
             { "SpriteSize", VFX::VFXValueType::Vec2, 1 },
             { "Color", VFX::VFXValueType::Vec4, 2 },
             { "SpriteRotation", VFX::VFXValueType::Float, 3 },
             { "Age", VFX::VFXValueType::Float, 4 },
             { "Lifetime", VFX::VFXValueType::Float, 5 },
             // The mesh renderer (UE UNiagaraMeshRendererProperties' Velocity / Scale / MeshOrientation bindings).
             { "Velocity", VFX::VFXValueType::Vec3, 8 },
             { "Scale", VFX::VFXValueType::Vec3, 9 },
             { "MeshOrientation", VFX::VFXValueType::Vec4, 10 },
             // The ribbon renderer (UE UNiagaraRibbonRendererProperties' RibbonWidth / RibbonID / RibbonFacing).
             { "RibbonWidth", VFX::VFXValueType::Float, 11 },
             { "RibbonID", VFX::VFXValueType::Int, 12 },
             { "RibbonFacing", VFX::VFXValueType::Vec3, 13 },
        };

        // Where the compiled stack put each drawn attribute; an attribute the stack declares with another type is
        // refused by name (drawn at its default), never read as the wrong number of components.
        ParticleDrawLayoutGpu DrawColumnsOf( const VFX::VFXCompiledEmitter& compiled, const uint32_t entityId,
                                             const std::string& emitter )
        {
            ParticleDrawLayoutGpu columns;
            for ( const DrawnAttribute& drawn : kDrawnAttributes )
            {
                const auto* a = compiled.Layout.Find( drawn.Name );
                if ( a == nullptr )
                    continue;
                if ( a->Type != drawn.Type )
                {
                    LOG_ERROR(
                         "[VFX] entity {} emitter '{}' draws {} at its default: the stack declares it {}, the "
                         "sprite renderer reads {}",
                         entityId, emitter, drawn.Name, VFX::GlslTypeName( a->Type ),
                         VFX::GlslTypeName( drawn.Type ) );
                    continue;
                }
                glm::uvec4* const rows[] = { &columns.Columns0, &columns.Columns1, &columns.Columns2,
                                             &columns.Columns3 };
                ( *rows[drawn.Lane / 4] )[drawn.Lane % 4] =
                     drawn.Type == VFX::VFXValueType::Int ? a->IntStart : a->FloatStart;
            }
            return columns;
        }

        // The bytes Generated/VFXEmitterStack.glslh becomes for one compiled stack: its Particle block, then where
        // the host finds the Position it places a newborn by.
        Common::ResultStr<std::string> StackHostSource( const VFX::VFXCompiledEmitter& compiled )
        {
            const auto parsed = Core::Preprocess::DShaderParser::Parse( compiled.ShaderText );
            if ( !parsed.IsSuccess() )
                return Common::MakeError<std::string>( parsed.GetError() );
            std::string source = parsed.GetValue().Meta.ParticleSource;
            source += '\n';
            if ( const auto* a = compiled.Layout.Find( "Position" );
                 a != nullptr && a->Type == VFX::VFXValueType::Vec3 )
                source += std::format( "#define VFX_ATTR_POSITION {}u\n", a->FloatStart );
            // VFX-PARAMS: the velocity an event carries (Common/VFXEvents.glslh).
            if ( const auto* a = compiled.Layout.Find( "Velocity" );
                 a != nullptr && a->Type == VFX::VFXValueType::Vec3 )
                source += std::format( "#define VFX_ATTR_VELOCITY {}u\n", a->FloatStart );
            return Common::MakeSuccess( std::move( source ) );
        }

        std::shared_ptr<ShaderResources::StorageBuffer> Columns( const char* name, uint32_t capacity,
                                                                 uint32_t columns, uint32_t binding )
        {
            return ShaderResources::StorageBuffer::Create( name, std::max( 1u, capacity * columns ) * 4u, binding,
                                                           /*persistent=*/true );
        }
    } // namespace

    ParticleEventLayout BuildParticleEventLayout( const Assets::Serialization::VFXSystemData& system,
                                                  const std::vector<uint32_t>&                capacities )
    {
        using Assets::Serialization::VFXEventHandler;
        struct Channel
        {
            std::size_t Source = 0;
            uint32_t    Kind   = 0;
        };
        std::vector<Channel> channels;
        const auto           channelOf = [&channels]( const std::size_t source, const uint32_t kind )
        {
            for ( uint32_t c = 0; c < static_cast<uint32_t>( channels.size() ); ++c )
                if ( channels[c].Source == source && channels[c].Kind == kind )
                    return c;
            channels.push_back( { source, kind } );
            return static_cast<uint32_t>( channels.size() - 1 );
        };
        const auto sourceOf = [&system]( const VFXEventHandler& handler )
        {
            for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                if ( system.Emitters[k].Name == handler.SourceEmitter )
                    return k;
            return system.Emitters.size(); // ValidateVFXSystemData refused such a file
        };

        // Every consumed (source, kind) once, in the order the handlers name them.
        std::vector<std::vector<std::pair<uint32_t, const VFXEventHandler*>>> handlers( system.Emitters.size() );
        for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
            for ( const VFXEventHandler& handler : system.Emitters[k].EventHandlers )
            {
                const std::size_t source = sourceOf( handler );
                if ( source < system.Emitters.size() )
                    handlers[k].emplace_back( channelOf( source, static_cast<uint32_t>( handler.Event ) ),
                                              &handler );
            }

        // Headers first (two per channel), then each channel's two halves of rows.
        std::vector<glm::uvec4> descriptors( channels.size() );
        uint32_t                cursor = 2u * static_cast<uint32_t>( channels.size() );
        for ( uint32_t c = 0; c < static_cast<uint32_t>( channels.size() ); ++c )
        {
            const uint32_t capacity = channels[c].Source < capacities.size() ? capacities[channels[c].Source] : 0u;
            descriptors[c]          = glm::uvec4( 2u * c, cursor, capacity, 0u );
            cursor += 2u * capacity * ( kParticleEventRowSize / 16u );
        }

        ParticleEventLayout layout;
        layout.BufferRows = std::max( 1u, cursor );
        layout.Tables.resize( system.Emitters.size() );
        for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
        {
            std::vector<glm::uvec4>& table = layout.Tables[k];
            table.assign( kParticleEventTableHandlers + 2u * handlers[k].size(), glm::uvec4( 0u ) );
            for ( uint32_t c = 0; c < static_cast<uint32_t>( channels.size() ); ++c )
                if ( channels[c].Source == k )
                {
                    table[0][static_cast<int>( channels[c].Kind )] = 1u;
                    table[1u + channels[c].Kind]                   = descriptors[c];
                }
            table[0].w = static_cast<uint32_t>( handlers[k].size() );
            for ( std::size_t h = 0; h < handlers[k].size(); ++h )
            {
                const auto& [channel, handler]              = handlers[k][h];
                glm::uvec4 row                              = descriptors[channel];
                row.w                                       = handler->SpawnCount;
                table[kParticleEventTableHandlers + 2u * h] = row;
                table[kParticleEventTableHandlers + 2u * h + 1u] =
                     glm::uvec4( std::bit_cast<uint32_t>( handler->InheritVelocity ), 0u, 0u, 0u );
            }
        }
        return layout;
    }

    void ParticleWorldGpu::BuildSystemEvents( ParticleSystemGpu& system, const uint32_t entityId )
    {
        std::vector<uint32_t> capacities;
        capacities.reserve( system.Emitters.size() );
        for ( const ParticleEmitterGpu& gpu : system.Emitters )
            capacities.push_back( gpu.Capacity );
        const ParticleEventLayout layout = BuildParticleEventLayout( *system.System, capacities );

        // GPU-written and read the step after, across frames: persistent, zeroed once (every half holds serial 0).
        system.EventSerial = 1;
        system.Events      = ShaderResources::StorageBuffer::Create(
             "VFXEvents", layout.BufferRows * static_cast<uint32_t>( sizeof( glm::uvec4 ) ), 11,
             /*persistent=*/true );
        if ( system.Events )
        {
            const std::vector<glm::uvec4> zero( layout.BufferRows, glm::uvec4( 0u ) );
            if ( const auto zeroed = system.Events->SetData(
                      zero.data(), static_cast<uint32_t>( zero.size() * sizeof( glm::uvec4 ) ) );
                 !zeroed.IsSuccess() )
            {
                LOG_ERROR( "[VFX] entity {}: the system's event buffer was not cleared, its emitters sit out: {}",
                           entityId, zeroed.GetError() );
                system.Events = nullptr;
            }
        }
        for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
        {
            ParticleEmitterGpu& gpu = system.Emitters[k];
            gpu.EventTableRows      = layout.Tables[k];
            gpu.Events              = system.Events;
            gpu.EventTable          = ShaderResources::StorageBuffer::Create(
                 "VFXEventTable", static_cast<uint32_t>( gpu.EventTableRows.size() * sizeof( glm::uvec4 ) ), 10 );
            if ( gpu.Capacity > 0 && ( !gpu.Events || !gpu.EventTable ) )
            {
                LOG_ERROR( "[VFX] entity {} emitter {} '{}' does not simulate: its event buffers were not created",
                           entityId, k, system.System->Emitters[k].Name );
                gpu.Capacity = 0;
                gpu.Pipeline = nullptr;
            }
        }
    }

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
        return e.Steps && e.Counters && e.DispatchArgs && e.ChannelSpawns && e.EventTable && e.Events &&
               e.AttributeFloats && e.AttributeInts && e.Params && e.Curves &&
               std::ranges::all_of( e.DrawLayouts, []( const auto& layout ) { return layout != nullptr; } );
    }

    std::shared_ptr<ComputePipeline> ParticleWorldGpu::ProgramFor( const uint64_t key, const std::string& name,
                                                                   const std::string& hostSource )
    {
        if ( const auto found = m_Programs.find( key ); found != m_Programs.end() )
            return found->second;
        std::shared_ptr<ComputePipeline>& program = m_Programs[key];
        auto*                             shaders = Runtime::ResourceRegistry::GetShaderService();
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

    void ParticleWorldGpu::BuildEmitter( ParticleEmitterGpu&                         gpu,
                                         const Assets::Serialization::VFXSystemData& system,
                                         const std::size_t index, const uint32_t entityId )
    {
        // VFXS 2: every enabled renderer row is one draw (UE FNiagaraRenderer per renderer properties).
        gpu.Index = static_cast<uint32_t>( index );
        gpu.Renderers.clear();
        gpu.RendererParams.clear();
        const auto& rows = system.Emitters[index].Renderers;
        for ( uint32_t row = 0; row < static_cast<uint32_t>( rows.size() ); ++row )
        {
            const Assets::Serialization::VFXRendererData& r = rows[row];
            if ( !r.Enabled )
                continue;
            ParticleRendererGpu drawn;
            drawn.Kind     = r.Kind;
            drawn.Row      = row;
            drawn.Material = Assets::Serialization::VFXRendererMaterialHandle( r );
            drawn.Mesh     = Assets::Serialization::VFXRendererMeshHandle( r );
            if ( r.Mesh )
            {
                drawn.MeshFacing = r.Mesh->Facing;
                drawn.MeshPath   = r.Mesh->Mesh.Path;
            }
            if ( r.Ribbon )
            {
                drawn.RibbonFacing   = r.Ribbon->Facing;
                drawn.RibbonUVTiling = r.Ribbon->UVTiling;
            }
            gpu.Renderers.push_back( drawn );
            gpu.RendererParams.push_back( ShaderResources::StorageBuffer::Create(
                 "ParticleRendererParams", kParticleRendererParamsSize, 14 ) );
        }

        const auto& emitter = system.Emitters[index];
        gpu.Capacity        = emitter.Enabled ? emitter.Capacity : 0u;
        gpu.Local           = emitter.Space == Assets::Serialization::VFXSimulationSpace::Local;
        gpu.LocalBounds     = Common::Math::AABB{ emitter.Bounds.Min, emitter.Bounds.Max };
        if ( gpu.Capacity == 0 )
            return;
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[VFX] entity {} emitter {} '{}' does not simulate: {}", entityId, index, emitter.Name,
                       why );
            gpu.Capacity = 0;
            gpu.Pipeline = nullptr;
        };

        const auto compiled = VFX::CompileEmitterStack( system, index, Assets::RegistryVFXModuleResolver{} );
        if ( !compiled.IsSuccess() )
            return refuse( compiled.GetError() );
        const auto curves = VFX::BuildCurveAtlas( system );
        if ( !curves.IsSuccess() )
            return refuse( curves.GetError() );
        auto params = VFX::BuildEmitterParams( compiled.GetValue(), system, index, curves.GetValue() );
        if ( !params.IsSuccess() )
            return refuse( params.GetError() );
        auto userRows = VFX::UserParamRowsOf( compiled.GetValue(), system );
        if ( !userRows.IsSuccess() )
            return refuse( userRows.GetError() );
        const auto source = StackHostSource( compiled.GetValue() );
        if ( !source.IsSuccess() )
            return refuse( source.GetError() );
        gpu.UserRows = std::move( userRows.GetValue() );

        gpu.Pipeline  = ProgramFor( compiled.GetValue().Key, compiled.GetValue().ShaderName, source.GetValue() );
        gpu.TickStage = compiled.GetValue().TickStage;
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
        gpu.DrawColumns = DrawColumnsOf( compiled.GetValue(), entityId, emitter.Name );
        for ( auto& layout : gpu.DrawLayouts )
            layout = ShaderResources::StorageBuffer::Create( "ParticleDrawLayout", kParticleDrawLayoutSize, 11 );
        gpu.NeedsReset = true;
    }

    bool ParticleWorldGpu::EnsurePoolCapacity( const uint32_t particles )
    {
        if ( particles <= m_Pool.Capacity && m_Pool.Slots )
            return true;
        // Grown to at least double, so a scene adding emitters one by one recreates the pool a logarithmic number
        // of times. The contents are not carried over: every emitter restarts (compact 0 resets its range).
        const uint32_t capacity = std::max( particles, m_Pool.Capacity * 2u );
        m_Pool                  = {};
        m_Pool.Slots = ShaderResources::StorageBuffer::Create( "ParticleSlots", capacity * kParticleSlotStride, 1,
                                                               /*persistent=*/true );
        m_Pool.FreeList  = ShaderResources::StorageBuffer::Create( "ParticleFreeList", capacity * 4u, 2, true );
        // Two alive lists per range (ping-pong halves): a compact reads the half the previous compact wrote.
        m_Pool.AliveList = ShaderResources::StorageBuffer::Create( "ParticleAliveList", capacity * 8u, 2, true );
        if ( !m_Pool.Slots || !m_Pool.FreeList || !m_Pool.AliveList )
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
                     // Emitters linked by events tick in one stage (VFX::UnifyEventTickStages): the event
                     // halves are ordered by step only inside one stage's chain.
                     std::vector<VFX::VFXTickStage> stages;
                     stages.reserve( system.Emitters.size() );
                     for ( const ParticleEmitterGpu& gpu : system.Emitters )
                         stages.push_back( gpu.TickStage );
                     VFX::UnifyEventTickStages( *instances->System, stages );
                     for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
                         system.Emitters[k].TickStage = stages[k];
                     BuildSystemEvents( system, entityId );
                 }

                 // One range of the pool per system, each emitter a slice of it in emitter order.
                 uint32_t total = 0;
                 for ( const ParticleEmitterGpu& gpu : system.Emitters )
                     total += gpu.Capacity;
                 if ( total == 0 )
                     return;
                 const ParticlePoolRange systemRange = m_Ranges.Acquire( entityId, total );
                 const glm::vec3         worldPos    = glm::vec3( transform.GetTransform()[3] );

                 uint32_t offset     = 0;
                 uint32_t eventSteps = 0; // the system's steps this tick: its event serial moves by them
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
                     // This placement's User.* values (VFXWorld resolved the component's overrides this tick).
                     VFX::ApplyUserParamRows( gpu.ParamRows, gpu.UserRows, instances->User );
                     eventSteps = std::max( eventSteps, std::min( static_cast<uint32_t>( instance->Steps.size() ),
                                                                  gpu.StepCapacity ) );
                     if ( !PrepareEmitter( gpu, *instance, range, entityId, worldPos, stepSeconds, stepSerial,
                                           system.EventSerial ) )
                         continue;
                 }
                 system.EventSerial += eventSteps;
             } );

        if ( !m_FrameEmitters.empty() && !EnsurePoolCapacity( m_Ranges.End() ) )
            m_FrameEmitters.clear();
        return true;
    }

    bool ParticleWorldGpu::PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                                           const ParticlePoolRange& range, const uint32_t entityId,
                                           const glm::vec3& worldPos, const float stepSeconds,
                                           const uint32_t stepSerial, const uint32_t eventSerial )
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

        uint32_t stepCount = std::min( static_cast<uint32_t>( instance->Steps.size() ), gpu.StepCapacity );
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
            const auto events = gpu.EventTable->SetData(
                 gpu.EventTableRows.data(),
                 static_cast<uint32_t>( gpu.EventTableRows.size() * sizeof( glm::uvec4 ) ) );
            if ( !uploaded.IsSuccess() || !params.IsSuccess() || !curves.IsSuccess() || !events.IsSuccess() )
            {
                LOG_ERROR( "[Particles] emitter {} does not simulate this tick, its step table, parameters, "
                           "curves or event table did not upload: {}",
                           entityId,
                           !uploaded.IsSuccess()
                                ? uploaded.GetError()
                                : ( !params.IsSuccess()
                                         ? params.GetError()
                                         : ( !curves.IsSuccess() ? curves.GetError() : events.GetError() ) ) );
                stepCount = 0;
            }
        }

        // This tick's Spawn from Channel particles, one record per particle (VFX-10): step s's spawn t <
        // ChannelCount reads record ChannelFirst + t. Grown x2 when a tick brings more.
        std::vector<ParticleChannelSpawnGpu> channel;
        for ( const VFX::VFXChannelSpawnRequest& r : instance->ChannelSpawns )
            for ( uint32_t k = 0; k < r.Count; ++k )
                channel.push_back( { glm::vec4( r.Position, r.HasPosition ? 1.0f : 0.0f ),
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
                LOG_ERROR( "[Particles] emitter {} sits out this tick, the buffer of its {} channel spawns "
                           "was not created",
                           entityId, needed );
                return false;
            }
            const auto uploaded =
                 gpu.ChannelSpawns->SetData( channel.data(), needed * kParticleChannelSpawnStride );
            if ( !uploaded.IsSuccess() )
            {
                LOG_ERROR( "[Particles] emitter {} does not simulate this tick, its {} channel spawns did "
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
            LOG_ERROR( "[Particles] emitter {} sits out this tick, its counters did not upload: {}", entityId,
                       counted.GetError() );
            return false;
        }

        // Where this tick's draw finds the particles: the compiled columns, the range they are strided by, and the
        // emitter position a Local emitter's particles are drawn around (the one the simulation placed them by).
        ParticleDrawLayoutGpu layout = gpu.DrawColumns;
        // w = the slot the last compact (index StepCount) fills: the ribbon draw reads its alive count from it.
        layout.Range = glm::uvec4( range.Count, range.Base, gpu.Local ? 1u : 0u, stepCount & 1u );
        layout.EmitterPos            = glm::vec4( worldPos, 0.0f );
        // Where the emitter stood at the tick before (a Local emitter's particles' previous world position is
        // taken around it); a fresh or restarted emitter has no previous tick: its particles are all born this
        // one.
        const bool hasPrevious = gpu.HasDrawnPos && !gpu.NeedsReset;
        layout.PrevEmitterPos  = glm::vec4( hasPrevious ? gpu.DrawnEmitterPos : worldPos, 0.0f );
        // Previous: the layout the last tick drew its particles by (half, slot, emitter positions), read by the
        // draws recorded before a PostOpaque emitter's simulation; with no previous tick nothing reads it
        // (ParticleFrameEmitter::HasPrevious), and it is this tick's so the buffer never holds stale ranges.
        const bool                   hasPreviousTick = gpu.HasLastHalf && !gpu.NeedsReset;
        const ParticleDrawLayoutGpu& previous        = hasPreviousTick ? gpu.LastLayout : layout;
        auto drawn = gpu.DrawLayouts[static_cast<std::size_t>( ParticleDrawTick::Current )]->SetData(
             &layout, kParticleDrawLayoutSize );
        if ( drawn.IsSuccess() )
            drawn = gpu.DrawLayouts[static_cast<std::size_t>( ParticleDrawTick::Previous )]->SetData(
                 &previous, kParticleDrawLayoutSize );
        if ( !drawn.IsSuccess() )
        {
            LOG_ERROR( "[Particles] emitter {} sits out this tick, its draw layout did not upload: {}", entityId,
                       drawn.GetError() );
            return false;
        }
        gpu.DrawnEmitterPos = worldPos;
        gpu.HasDrawnPos     = true;
        gpu.LastLayout      = layout;

        for ( std::size_t r = 0; r < gpu.Renderers.size(); ++r )
        {
            const ParticleRendererGpu&      row    = gpu.Renderers[r];
            const uint32_t                  facing = row.Kind == Assets::Serialization::VFXRendererKind::Mesh
                                                          ? static_cast<uint32_t>( row.MeshFacing )
                                                          : static_cast<uint32_t>( row.RibbonFacing );
            const ParticleRendererParamsGpu params{
                 glm::uvec4( facing, 0u, 0u, 0u ),
                 glm::vec4( row.RibbonUVTiling, static_cast<float>( stepCount ) * stepSeconds, 0.0f, 0.0f ) };
            if ( const auto set = gpu.RendererParams[r]->SetData( &params, kParticleRendererParamsSize );
                 !set.IsSuccess() )
            {
                LOG_ERROR(
                     "[Particles] emitter {} sits out this tick, renderer row {}'s settings did not upload: {}",
                     entityId, row.Row, set.GetError() );
                return false;
            }
        }

        ParticleFrameEmitter fe;
        fe.EntityId        = entityId;
        fe.Gpu             = &gpu;
        fe.Pipeline        = gpu.Pipeline.get();
        fe.StepCount       = stepCount;
        fe.PreviousHalf    = gpu.LastHalf;
        fe.HasPrevious     = gpu.HasLastHalf && !gpu.NeedsReset;
        gpu.LastHalf       = stepCount & 1u;
        gpu.HasLastHalf    = true;
        fe.Push.EmitterPos = glm::vec4( worldPos, stepSeconds );
        // The placement is the position alone (the simulation reads no rotation or scale), so the box translates.
        fe.WorldBounds     = Common::Math::AABB{ gpu.LocalBounds.Min + worldPos, gpu.LocalBounds.Max + worldPos };
        fe.Push.Counts     = glm::uvec4( range.Count, 0u, range.Base, gpu.Local ? 1u : 0u );
        fe.Push.Stack      = glm::uvec4( stepSerial, eventSerial, 0u, 0u );
        m_FrameEmitters.push_back( fe );
        return true;
    }

} // namespace Desert::Graphic::System

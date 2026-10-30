#include "ParticleWorldGpu.hpp"
#include <algorithm>
#include <bit>

#include "ParticleEmitterRetire.hpp"

#include <Engine/Assets/VFXModuleAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LightUnits.hpp>
#include <Engine/VFX/VFXWorld.hpp>
#include <Engine/VFX/VFXCurveLUT.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
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
        // THE ONE TABLE of the attributes a renderer reads (UE: the sprite renderer's attribute bindings), each
        // with the one type it reads it as and its lane in ParticleDrawLayoutGpu (Columns0.xyzw, Columns1.xyzw).
        // The sprite vertex path and the sort read them out of the emitter's columns through
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
             { "RibbonWidth", VFX::VFXValueType::Float, 6 },
             { "RibbonLinkOrder", VFX::VFXValueType::Float, 7 },
             // The Mesh row's (UE Particles.Scale, Particles.MeshOrientation): Columns2.y/z (Columns2.x is the
             // Ribbon row's RibbonIdBinding, an int column).
             { "Scale", VFX::VFXValueType::Vec3, 9 },
             { "MeshOrientation", VFX::VFXValueType::Vec4, 10 },
             // A VelocityAligned Sprite row's axis (UE Particles.Velocity in the sprite vertex factory): Columns2.w.
             { "Velocity", VFX::VFXValueType::Vec3, 11 },
        };

        // Where the compiled stack put each drawn attribute; an attribute the stack declares with another type is
        // refused by name (drawn at its default), never read as the wrong number of components.
        ParticleDrawLayoutGpu DrawColumnsOf( const VFX::VFXCompiledEmitter& compiled, const uint32_t entityId,
                                             const std::string&                                         emitter,
                                             const std::vector<Assets::Serialization::VFXRendererData>& renderers )
        {
            ParticleDrawLayoutGpu columns;
            // The Ribbon row's RibbonIdBinding (UE: the ribbon renderer's RibbonId binding, an int): the int column
            // ParticleSort splits the emitter's ribbons by. An unbound or mistyped name is refused aloud and the
            // emitter draws one ribbon, as every drawn attribute falls back to its default.
            for ( const Assets::Serialization::VFXRendererData& row : renderers )
            {
                if ( !row.Enabled || row.Kind != Assets::Serialization::VFXRendererKind::Ribbon || !row.Ribbon )
                    continue;
                const std::string& binding = row.Ribbon->RibbonIdBinding;
                if ( binding.empty() )
                    break;
                const std::string  name    = binding.starts_with( Assets::Serialization::kVFXParticlesPrefix )
                                                  ? binding.substr( Assets::Serialization::kVFXParticlesPrefix.size() )
                                                  : binding;
                const auto*        a       = compiled.Layout.Find( name );
                if ( a == nullptr || a->Type != VFX::VFXValueType::Int )
                {
                    LOG_ERROR( "[VFX] entity {} emitter '{}' draws one ribbon: its RibbonIdBinding '{}' {}", entityId,
                               emitter, binding,
                               a == nullptr ? "names no attribute of the stack" : "is not an Int attribute" );
                }
                else
                {
                    columns.Columns2.x = a->IntStart;
                }
                break;
            }
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
                ( drawn.Lane < 4   ? columns.Columns0
                  : drawn.Lane < 8 ? columns.Columns1
                                   : columns.Columns2 )[drawn.Lane % 4] = a->FloatStart;
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
            // The velocity an event carries (Common/VFXEvents.glslh).
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
        // Emitters linked by events tick in one stage: a source in the other chain would run all its steps before
        // or after its handler's, and the handler would read a half overwritten or not yet written.
        std::vector<VFX::VFXTickStage> stages;
        stages.reserve( system.Emitters.size() );
        for ( const ParticleEmitterGpu& gpu : system.Emitters )
            stages.push_back( gpu.TickStage );
        VFX::UnifyEventTickStages( *system.System, stages );
        for ( std::size_t k = 0; k < system.Emitters.size(); ++k )
            system.Emitters[k].TickStage = stages[k];

        std::vector<uint32_t> capacities;
        capacities.reserve( system.Emitters.size() );
        for ( const ParticleEmitterGpu& gpu : system.Emitters )
            capacities.push_back( gpu.Capacity );
        const ParticleEventLayout layout = BuildParticleEventLayout( *system.System, capacities );

        // GPU-written and read the step after, across frames: persistent, zeroed once (every half holds serial 0).
        system.EventSerial = 1;
        system.Events      = ShaderResources::StorageBuffer::Create(
             "VFXEvents", layout.BufferRows * static_cast<uint32_t>( sizeof( glm::uvec4 ) ), 15,
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
                 "VFXEventTable", static_cast<uint32_t>( gpu.EventTableRows.size() * sizeof( glm::uvec4 ) ), 14 );
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
            // PERSISTENT, never written by the host: each chain's "Open" node writes both slots on the GPU
            // (ParticleRenderer::OpenDrawSlots), so the slot the previous tick's last compact filled survives into
            // this frame until that node - what a PostOpaque emitter's draws recorded before its simulation read.
            e.Counters = ShaderResources::StorageBuffer::Create(
                 "ParticleCounters", kParticleDrawSlots * kParticleDrawSlotStride, 1, /*persistent=*/true );
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
               e.AttributeFloats &&
               e.AttributeInts && e.Params && e.Curves && e.PointSets && e.DrawLayout;
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
        auto acquired = shaders->AcquireVariant( "ParticleSimulate", variant );
        if ( !acquired )
        {
            if ( shaders->ClaimRefusalReport( "ParticleSimulate", variant ) )
                LOG_ERROR( "[VFX] stack {} has no host program: {}", name, acquired.GetError() );
            return nullptr;
        }
        const auto shader = acquired.ExtractValue();
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
        const auto& emitter = system.Emitters[index];
        gpu.Capacity        = emitter.Enabled ? emitter.Capacity : 0u;
        gpu.Local           = emitter.Space == Assets::Serialization::VFXSimulationSpace::Local;
        gpu.DebugEntity     = entityId;
        gpu.DebugEmitter    = static_cast<uint32_t>( index );
        gpu.DebugName       = emitter.Name;
        gpu.DebugReadback.reset();
        gpu.DebugColumns.clear();
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
        const auto source = StackHostSource( compiled.GetValue() );
        if ( !source.IsSuccess() )
            return refuse( source.GetError() );

        gpu.Pipeline  = ProgramFor( compiled.GetValue().Key, compiled.GetValue().ShaderName, source.GetValue() );
        gpu.TickStage       = compiled.GetValue().TickStage;
        gpu.ReadsSceneDepth = gpu.TickStage == VFX::VFXTickStage::PostOpaque; // before UnifyEventTickStages
        if ( !gpu.Pipeline )
            return refuse( "its host program did not build (logged above)" );
        gpu.ParamRows    = std::move( params.GetValue() );
        gpu.UserRows.clear();
        for ( std::size_t s = 0; s < compiled.GetValue().Slots.size(); ++s )
            if ( const VFX::VFXParamSlot& slot = compiled.GetValue().Slots[s];
                 slot.SlotKind == VFX::VFXParamSlot::Kind::User )
                gpu.UserRows.emplace_back( static_cast<uint32_t>( s ), slot.User );
        gpu.PointSetRows.clear();
        for ( std::size_t s = 0; s < compiled.GetValue().Slots.size(); ++s )
            if ( const VFX::VFXParamSlot& slot = compiled.GetValue().Slots[s];
                 slot.SlotKind == VFX::VFXParamSlot::Kind::PointSet )
                gpu.PointSetRows.emplace_back( static_cast<uint32_t>( s ), slot.User );
        gpu.CurveFloats  = curves.GetValue().Floats;
        gpu.FloatColumns = compiled.GetValue().Layout.TotalFloatComponents;
        gpu.IntColumns   = compiled.GetValue().Layout.TotalIntComponents;
        gpu.DebugColumns = compiled.GetValue().Layout.Attributes;
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
        gpu.PointSetData.assign( 1, glm::vec4( 0.0f ) );
        gpu.PointSets   = ShaderResources::StorageBuffer::Create( "VFXPointSets", sizeof( glm::vec4 ), 10 );
        gpu.DrawColumns = DrawColumnsOf( compiled.GetValue(), entityId, emitter.Name, emitter.Renderers );
        gpu.DrawLayout =
             ShaderResources::StorageBuffer::Create( "ParticleDrawLayout", kParticleDrawLayoutSize, 11 );
        gpu.NeedsReset = true;
    }

    void ParticleWorldGpu::FillDebugStats( VFX::VFXDebugStats& stats ) const
    {
        stats.GpuCounts = DebugReading();
        for ( VFX::VFXSystemDebugStats& system : stats.Systems )
            for ( VFX::VFXEmitterDebugStats& emitter : system.Emitters )
                for ( const ParticleEmitterDebug& read : m_Debug )
                    if ( read.EntityId == system.EntityId && read.Emitter == emitter.Emitter )
                    {
                        emitter.GpuRead   = read.Read;
                        emitter.Alive     = read.Alive;
                        emitter.Free      = read.Free;
                        emitter.FramesOld = read.FramesOld;
                        break;
                    }
    }

    void ParticleWorldGpu::ReadDebugReadbacks()
    {
        m_Debug.clear();
        if ( !DebugReading() )
            return;
        m_DebugSerial = m_DebugSerial == 0xFFFFFFFFu ? 1u : m_DebugSerial + 1u; // 0 = a copy never written
        m_Debug.reserve( m_FrameEmitters.size() );
        for ( const ParticleFrameEmitter& fe : m_FrameEmitters )
        {
            ParticleEmitterGpu&  gpu = *fe.Gpu;
            ParticleEmitterDebug debug;
            debug.EntityId     = fe.EntityId;
            debug.Emitter      = gpu.DebugEmitter;
            debug.Name         = gpu.DebugName;
            debug.Capacity     = gpu.Range.Count;
            debug.FloatColumns = gpu.FloatColumns;
            debug.IntColumns   = gpu.IntColumns;
            const uint32_t stride = 1u + gpu.FloatColumns + gpu.IntColumns;
            const uint32_t words  = kParticleDebugHeaderUints + kParticleDebugMaxRows * stride;
            if ( !gpu.DebugReadback )
            {
                // Its copies are written from this frame on; the first comes back FramesInFlight frames later.
                gpu.DebugReadback = ShaderResources::StorageBuffer::Create( "ParticleDebugReadback", words * 4u, 3 );
                m_Debug.push_back( std::move( debug ) );
                continue;
            }
            std::vector<uint32_t> data( words, 0u );
            if ( const Common::BoolResultStr read = gpu.DebugReadback->ReadBack( data.data(), words * 4u ); !read )
            {
                LOG_ERROR( "[Particles] entity {} emitter {} '{}': the debug readback was not read: {}", fe.EntityId,
                           gpu.DebugEmitter, gpu.DebugName, read.GetError() );
                m_Debug.push_back( std::move( debug ) );
                continue;
            }
            if ( data[2] != 0u )
            {
                debug.Read      = true;
                debug.Alive     = data[0];
                debug.Free      = data[1];
                debug.FramesOld = m_DebugSerial - data[2];
                if ( IsDebugTable( gpu ) )
                {
                    const uint32_t rows = std::min( data[3], kParticleDebugMaxRows );
                    debug.Columns       = gpu.DebugColumns;
                    debug.RowIndex.reserve( rows );
                    debug.Floats.reserve( std::size_t{ rows } * gpu.FloatColumns );
                    debug.Ints.reserve( std::size_t{ rows } * gpu.IntColumns );
                    for ( uint32_t r = 0; r < rows; ++r )
                    {
                        const uint32_t base = kParticleDebugHeaderUints + r * stride;
                        debug.RowIndex.push_back( data[base] );
                        for ( uint32_t c = 0; c < gpu.FloatColumns; ++c )
                            debug.Floats.push_back( std::bit_cast<float>( data[base + 1u + c] ) );
                        for ( uint32_t c = 0; c < gpu.IntColumns; ++c )
                            debug.Ints.push_back( std::bit_cast<int32_t>( data[base + 1u + gpu.FloatColumns + c] ) );
                    }
                }
            }
            m_Debug.push_back( std::move( debug ) );
        }
    }

    void ParticleWorldGpu::ReadLightReadbacks()
    {
        m_Lights.clear();
        m_LightSerial = m_LightSerial == 0xFFFFFFFFu ? 1u : m_LightSerial + 1u; // 0 = a copy never written
        constexpr uint32_t words =
             kParticleLightHeaderUints + Assets::Serialization::kVFXLightMaxPerEmitter * kParticleLightRowFloats;
        std::vector<uint32_t> data( words, 0u );
        for ( const ParticleFrameEmitter& fe : m_FrameEmitters )
        {
            if ( fe.Light == nullptr || !fe.Gpu->LightReadback )
                continue;
            std::fill( data.begin(), data.end(), 0u );
            if ( const Common::BoolResultStr read = fe.Gpu->LightReadback->ReadBack( data.data(), words * 4u );
                 !read )
            {
                LOG_ERROR( "[Particles] entity {} emitter {}: the light readback was not read: {}", fe.EntityId,
                           fe.Emitter, read.GetError() );
                continue;
            }
            if ( data[1] == 0u )
                continue; // this copy was not written yet (the first FramesInFlight frames)
            const Assets::Serialization::VFXLightRendererData& l = *fe.Light;
            const uint32_t                                     rows =
                 std::min( { data[0], l.MaxLights, Assets::Serialization::kVFXLightMaxPerEmitter } );
            for ( uint32_t r = 0; r < rows; ++r )
            {
                const auto at = [&data, r]( const uint32_t c ) {
                    return std::bit_cast<float>(
                         data[kParticleLightHeaderUints + r * kParticleLightRowFloats + c] );
                };
                const glm::vec4                    color( at( 4 ), at( 5 ), at( 6 ), at( 7 ) );
                ShaderProtocols::PointLightPayload light{};
                light.Position = glm::vec3( at( 0 ), at( 1 ), at( 2 ) );
                light.Radius   = std::max( at( 3 ), 0.0f ) * l.RadiusScale;
                light.Color =
                     glm::max( glm::vec3( color ) *
                                    ( l.AlphaScalesBrightness ? std::clamp( color.a, 0.0f, 1.0f ) : 1.0f ),
                               glm::vec3( 0.0f ) ) +
                     l.ColorAdd;
                // UE Niagara: inverse-square lights state candelas (the photometric law with no emitter size);
                // the exponent variant is the renderer's unitless multiplier.
                const ECS::LightFalloff falloff =
                     l.UseInverseSquaredFalloff ? ECS::LightFalloff::InverseSquare : ECS::LightFalloff::Quadratic;
                light.Falloff   = static_cast<int32_t>( falloff );
                light.Intensity = ECS::LightPhotometry::ShaderIntensity(
                     l.Intensity,
                     l.UseInverseSquaredFalloff ? ECS::LightUnits::Candelas : ECS::LightUnits::Unitless, falloff,
                     0.0f, ECS::LightPhotometry::kPointLightCosHalfCone );
                if ( light.Radius <= 0.0f || !std::isfinite( light.Radius ) ||
                     !std::isfinite( light.Position.x + light.Position.y + light.Position.z ) )
                    continue;
                m_Lights.push_back( light );
            }
        }
    }

    void ParticleWorldGpu::ApplyUserOverrides( ParticleSystemGpu& system,
                                               const std::vector<ECS::VFXUserOverride>& overrides,
                                               const uint32_t entityId )
    {
        // UE OverrideParameters: the placement's value replaces the row the stack's User binding reads; the
        // rows go up with the next step (PrepareEmitter uploads ParamRows every simulated tick).
        system.Overrides = overrides;
        const auto& rows = system.System->UserParams;
        for ( const ECS::VFXUserOverride& o : overrides )
            if ( std::none_of( rows.begin(), rows.end(),
                               [&]( const Assets::Serialization::VFXUserParam& p ) { return p.Name == o.Name; } ) )
                LOG_ERROR( "[VFX] entity {}: the override of User.{} names no User parameter of its system - it "
                           "plays nothing",
                           entityId, o.Name );
        for ( ParticleEmitterGpu& gpu : system.Emitters )
            for ( const auto& [row, name] : gpu.UserRows )
            {
                const auto it = std::find_if( rows.begin(), rows.end(),
                                              [&]( const Assets::Serialization::VFXUserParam& p )
                                              { return p.Name == name; } );
                if ( it != rows.end() && row < gpu.ParamRows.size() )
                    gpu.ParamRows[row] = Assets::Serialization::UserParamValue( *it, overrides );
            }
    }

    void ParticleWorldGpu::ApplyPointSets( ParticleSystemGpu& system, const std::vector<ECS::VFXPointSet>& sets,
                                           const uint32_t entityId )
    {
        // UE ArrayPosition: the set is the emitter's own buffer, every painted point in it (no cap), each with the
        // set's cumulative weight so a particle picks a point in proportion to its weight (VFX_PointSet).
        system.PointSets = sets;
        for ( ParticleEmitterGpu& gpu : system.Emitters )
        {
            gpu.PointSetData.clear();
            for ( const auto& [row, name] : gpu.PointSetRows )
            {
                const auto set = std::find_if( sets.begin(), sets.end(),
                                               [&]( const ECS::VFXPointSet& s ) { return s.Name == name; } );
                double total = 0.0;
                if ( set != sets.end() )
                    for ( const glm::vec4& p : set->Points )
                        total += std::max( p.w, 0.0f );
                const auto first = static_cast<uint32_t>( gpu.PointSetData.size() );
                if ( total <= 0.0 )
                {
                    LOG_WARN( "[VFX] entity {} emitter '{}': PointSet.{} {} - its particles are born at the "
                              "placement",
                              entityId, gpu.DebugName, name,
                              set == sets.end() ? "is not painted on this placement" : "holds no weighted point" );
                    if ( row < gpu.ParamRows.size() )
                        gpu.ParamRows[row] = glm::vec4( 0.0f );
                    continue;
                }
                double cumulative = 0.0;
                for ( const glm::vec4& p : set->Points )
                {
                    if ( p.w <= 0.0f )
                        continue; // a zero-weight point is never picked
                    cumulative += p.w;
                    gpu.PointSetData.emplace_back( glm::vec3( p ), static_cast<float>( cumulative / total ) );
                }
                gpu.PointSetData.back().w = 1.0f; // the last point takes u up to 1 whatever the rounding
                if ( row < gpu.ParamRows.size() )
                    gpu.ParamRows[row] =
                         glm::vec4( static_cast<float>( first ),
                                    static_cast<float>( gpu.PointSetData.size() - first ), 0.0f, 0.0f );
            }
            if ( gpu.PointSetData.empty() )
                gpu.PointSetData.emplace_back( 0.0f );
            const auto bytes = static_cast<uint32_t>( gpu.PointSetData.size() * sizeof( glm::vec4 ) );
            if ( !gpu.PointSets || gpu.PointSets->GetSize() < bytes )
                gpu.PointSets = ShaderResources::StorageBuffer::Create( "VFXPointSets", bytes, 10 );
        }
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
        m_DebugCounts              = world.GetDebugSettings().Overview;
        if ( !m_Claim.Claim( world.GetTickSerial() ) )
            return false; // another view of this scene simulates this tick; this one draws the same pool

        // THE SCENE'S ROWS. The pool, the emitters' buffers and the host programs belong to the scene's VFXWorld,
        // but this runs inside the simulating view's ledger scope (PrepareTick from the view's particle pass), so
        // without this they carried that view's instance and outlived its close as the view's rows
        // (VIEW-CLOSE-LEAK). Nested: the view's scope is restored when this one closes.
        const ResourceAttributionScope sceneRows( ResourceOwner::SceneRenderer, scene.GetLedgerInstance() );

        m_FrameEmitters.clear();

        const auto&    clock        = world.GetClock().GetSettings();
        const auto     stepSeconds  = static_cast<float>( clock.StepSeconds );
        const uint32_t stepCapacity = std::max( clock.MaxStepsPerTick, clock.MaxSeekStepsPerTick );

        const auto& reg = scene.GetRegistry();

        const std::size_t retired = RetireDestroyedEmitters( m_Systems, reg );
        if ( retired > 0 )
            LOG_INFO( "[Particles] Released {} system(s) whose entity is gone.", retired );
        m_Ranges.ReleaseUnless( [this]( const uint32_t key ) { return m_Systems.contains( key ); } );

        auto view = reg.view<const ECS::VFXComponent, const ECS::TransformComponent, const ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, const ECS::VFXComponent& vfx, const ECS::TransformComponent& transform,
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
                     BuildSystemEvents( system, entityId );
                     ApplyUserOverrides( system, vfx.Data.Overrides, entityId );
                     ApplyPointSets( system, vfx.Data.PointSets, entityId );
                 }
                 else
                 {
                     if ( system.Overrides != vfx.Data.Overrides )
                         ApplyUserOverrides( system, vfx.Data.Overrides, entityId );
                     if ( system.PointSets != vfx.Data.PointSets )
                         ApplyPointSets( system, vfx.Data.PointSets, entityId );
                 }

                 // One range of the pool per system, each emitter a slice of it in emitter order.
                 uint32_t total = 0;
                 for ( const ParticleEmitterGpu& gpu : system.Emitters )
                     total += gpu.Capacity;
                 if ( total == 0 )
                     return;
                 const ParticlePoolRange systemRange = m_Ranges.Acquire( entityId, total );
                 const glm::mat4         localToWorld( transform.GetTransform() );

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
                     eventSteps = std::max( eventSteps, std::min( static_cast<uint32_t>( instance->Steps.size() ),
                                                                  gpu.StepCapacity ) );
                     if ( !PrepareEmitter( gpu, *instance, instances->System->Emitters[k], range, entityId,
                                           static_cast<uint32_t>( k ), localToWorld, stepSeconds,
                                           system.EventSerial ) )
                         continue;
                 }
                 system.EventSerial += eventSteps;
             } );

        if ( !m_FrameEmitters.empty() && !EnsurePoolCapacity( m_Ranges.End() ) )
            m_FrameEmitters.clear();
        return true;
    }

    // THE ONE RULE for a renderer row's material (VFXS 2): the GUID the row states, or - the row states none - the
    // null handle, which every drawing path reads as the engine's particle material for the row's kind (UE: the
    // renderer properties' DefaultSpriteMaterial / DefaultRibbonMaterial; a Mesh row's absent override = the
    // mesh's own materials). A stated GUID that does not parse is refused here, not drawn with the default.
    static Common::ResultStr<Common::AssetHandle>
    ParticleRendererMaterial( const Assets::Serialization::VFXRendererData& row )
    {
        if ( !row.Material )
            return Common::MakeSuccess( Common::AssetHandle::Null() );
        const auto guid = Common::Content::AssetGuidFromText( row.Material->Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeError<Common::AssetHandle>(
                 std::format( "its Material GUID '{}' ({}) is not a well-formed asset GUID", row.Material->Guid,
                              row.Material->Path ) );
        return Common::MakeSuccess(
             Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) ) );
    }

    bool ParticleWorldGpu::PrepareEmitter( ParticleEmitterGpu& gpu, const VFX::EmitterInstance& emitter,
                                           const Assets::Serialization::VFXEmitterData& data,
                                           const ParticlePoolRange& range, const uint32_t entityId,
                                           const uint32_t emitterIndex, const glm::mat4& localToWorld,
                                           const float stepSeconds, const uint32_t eventSerial )
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
            auto curves = gpu.Curves->SetData( gpu.CurveFloats.data(),
                                               static_cast<uint32_t>( gpu.CurveFloats.size() * sizeof( float ) ) );
            if ( curves.IsSuccess() )
                curves = gpu.PointSets->SetData(
                     gpu.PointSetData.data(),
                     static_cast<uint32_t>( gpu.PointSetData.size() * sizeof( glm::vec4 ) ) );
            if ( curves.IsSuccess() )
                curves = gpu.EventTable->SetData(
                     gpu.EventTableRows.data(),
                     static_cast<uint32_t>( gpu.EventTableRows.size() * sizeof( glm::uvec4 ) ) );
            if ( !uploaded.IsSuccess() || !params.IsSuccess() || !curves.IsSuccess() )
            {
                LOG_ERROR( "[Particles] emitter {} does not simulate this tick, its step table, parameters, "
                           "curves, point sets or event table did not upload: {}",
                           entityId,
                           !uploaded.IsSuccess()
                                ? uploaded.GetError()
                                : ( !params.IsSuccess() ? params.GetError() : curves.GetError() ) );
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

        // The draw slots are opened on the GPU, not here: the emitter's chain starts with "Particles <stage>: Open"
        // (ParticleRenderer::OpenDrawSlots, ParticleDispatchArgs in its open mode), which writes slot h empty with
        // FirstVertex = 6 x (2 Base + h Count) for compact 0 to fill. Until that node runs the Counters still hold
        // the previous tick's slots (a PostOpaque emitter's draws before its simulation read them).

        // Where this tick's draw finds the particles: the compiled columns, the range they are strided by, and the
        // placement transform a Local emitter's particles are drawn through (the one the simulation placed them by).
        ParticleDrawLayoutGpu layout = gpu.DrawColumns;
        layout.Range                 = glm::uvec4( range.Count, range.Base, gpu.Local ? 1u : 0u, 0u );
        layout.LocalToWorld          = localToWorld;
        // The emitter's Ribbon row: its settings ride the layout to the ribbon vertex path (its RibbonIdBinding's
        // column is in DrawColumns). The emitter's ribbons are drawn by the first enabled Ribbon row; another with
        // other settings is refused aloud below (one ribbon order per emitter, ParticleSort).
        const Assets::Serialization::VFXRibbonRendererData* ribbon = nullptr;
        for ( const Assets::Serialization::VFXRendererData& row : data.Renderers )
        {
            if ( row.Enabled && row.Kind == Assets::Serialization::VFXRendererKind::Ribbon && row.Ribbon )
            {
                ribbon = &*row.Ribbon;
                break;
            }
        }
        // The emitter's Sprite row alignment (UE ENiagaraSpriteAlignment): the first enabled Sprite row's, as the
        // Ribbon row's settings above; an absent settings block is UE's default, Unaligned.
        for ( const Assets::Serialization::VFXRendererData& row : data.Renderers )
        {
            if ( row.Enabled && row.Kind == Assets::Serialization::VFXRendererKind::Sprite )
            {
                layout.Range.w = ( row.Sprite && row.Sprite->Alignment ==
                                                       Assets::Serialization::VFXSpriteAlignment::VelocityAligned )
                                      ? 1u
                                      : 0u;
                break;
            }
        }
        if ( ribbon != nullptr )
            layout.Ribbon =
                 glm::vec4( ribbon->Width,
                            ribbon->UVMode == Assets::Serialization::VFXRibbonUVMode::TileByDistance ? 1.0f : 0.0f,
                            ribbon->UVTileDistance, 0.0f );
        if ( const auto drawn = gpu.DrawLayout->SetData( &layout, kParticleDrawLayoutSize ); !drawn.IsSuccess() )
        {
            LOG_ERROR( "[Particles] emitter {} sits out this tick, its draw layout did not upload: {}", entityId,
                       drawn.GetError() );
            return false;
        }

        ParticleFrameEmitter fe;
        fe.EntityId        = entityId;
        fe.Emitter         = emitterIndex;
        fe.Gpu             = &gpu;
        fe.Pipeline        = gpu.Pipeline.get();
        fe.StepCount       = stepCount;
        // The half the previous tick's last compact filled (PostOpaque emitters' draws before their simulation);
        // none after a reset - the range starts dead.
        fe.PreviousHalf    = gpu.LastHalf;
        fe.HasPrevious     = gpu.HasLastHalf && !gpu.NeedsReset;
        gpu.LastHalf       = stepCount & 1u;
        gpu.HasLastHalf    = true;
        fe.Push.LocalToWorld = localToWorld;
        fe.Push.Step         = glm::vec4( stepSeconds, 0.0f, 0.0f, 0.0f );
        fe.WorldBounds       = Assets::Serialization::VFXBoundsAt( data.Bounds, localToWorld );
        fe.Push.Counts     = glm::uvec4( range.Count, 0u, range.Base, gpu.Local ? 1u : 0u );
        // The random streams' step (Stack.x + step): the emitter's own step since its reset, never the world's
        // tick - the same age draws the same numbers however it was reached (play, scrub forward, scrub back).
        // y: the system's event step serial of step 0 (Common/VFXEvents.glslh).
        fe.Push.Stack = glm::uvec4( instance->FirstStep, eventSerial, 0u, 0u );
        fe.Renderers.reserve( data.Renderers.size() );
        for ( std::size_t r = 0; r < data.Renderers.size(); ++r )
        {
            const Assets::Serialization::VFXRendererData& row = data.Renderers[r];
            if ( !row.Enabled )
                continue;
            if ( row.Kind == Assets::Serialization::VFXRendererKind::Light )
            {
                // VFX-LIGHT: a Light row draws nothing; its particles' lights come back through LightReadback.
                if ( !row.Light )
                    continue;
                if ( fe.Light != nullptr )
                {
                    LOG_ERROR( "[Particles] emitter {} renderer row {} casts no light: the emitter's first Light "
                               "row already does, and an emitter reads one set of lights",
                               entityId, r );
                    continue;
                }
                fe.Light = &*row.Light;
                if ( !gpu.LightReadback )
                    gpu.LightReadback = ShaderResources::StorageBuffer::Create(
                         "ParticleLightReadback",
                         ( kParticleLightHeaderUints +
                           Assets::Serialization::kVFXLightMaxPerEmitter * kParticleLightRowFloats ) *
                              4u,
                         3 );
                continue;
            }
            if ( row.Kind == Assets::Serialization::VFXRendererKind::Ribbon )
            {
                if ( !row.Ribbon || *row.Ribbon != *ribbon )
                {
                    LOG_ERROR( "[Particles] emitter {} renderer row {} does not draw: its Ribbon settings differ "
                               "from the emitter's first Ribbon row, and an emitter draws one ribbon order",
                               entityId, r );
                    continue;
                }
            }
            const auto material = ParticleRendererMaterial( row );
            if ( !material )
            {
                LOG_ERROR( "[Particles] emitter {} renderer row {} does not draw: {}", entityId, r,
                           material.GetError() );
                continue;
            }
            fe.Renderers.push_back( { row.Kind, static_cast<uint32_t>( r ), material.GetValue(),
                                      row.Ribbon ? &*row.Ribbon : nullptr, row.Mesh ? &*row.Mesh : nullptr } );
        }
        m_FrameEmitters.push_back( fe );
        return true;
    }

} // namespace Desert::Graphic::System

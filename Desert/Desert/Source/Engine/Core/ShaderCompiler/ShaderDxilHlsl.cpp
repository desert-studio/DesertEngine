#include "ShaderDxilHlsl.hpp"

#include <Engine/Graphic/Shader.hpp>

#include <spirv_hlsl.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        // One Vulkan (set, binding) of the program, merged across the stages that declare it.
        struct DxilSlot
        {
            uint32_t      Set     = 0;
            uint32_t      Binding = 0;
            DxilRangeType Type    = DxilRangeType::Srv;
            bool          Paired  = false; // a combined image sampler: an SRV here and a sampler at the same slot
            uint32_t      Count   = 1;     // kDxilUnboundedCount: a runtime array
            uint32_t      Stages  = 0;     // Formats::ShaderStage bits of the stages that declare it
            std::string   Name;
            std::string   DeclaringStage; // the first stage, for the refusal that names both
        };

        DxilShaderVisibility DxilVisibilityOf( const uint32_t stageBits )
        {
            switch ( static_cast<Formats::ShaderStage>( stageBits ) )
            {
                case Formats::ShaderStage::Vertex:
                    return DxilShaderVisibility::Vertex;
                case Formats::ShaderStage::TessControl:
                    return DxilShaderVisibility::Hull;
                case Formats::ShaderStage::TessEvaluation:
                    return DxilShaderVisibility::Domain;
                case Formats::ShaderStage::Fragment:
                    return DxilShaderVisibility::Pixel;
                default:
                    return DxilShaderVisibility::All; // several stages, or compute (compute ignores visibility)
            }
        }

        bool DxilExecutionModelOf( const Formats::ShaderStage stage, spv::ExecutionModel& model )
        {
            switch ( stage )
            {
                case Formats::ShaderStage::Vertex:
                    model = spv::ExecutionModelVertex;
                    return true;
                case Formats::ShaderStage::TessControl:
                    model = spv::ExecutionModelTessellationControl;
                    return true;
                case Formats::ShaderStage::TessEvaluation:
                    model = spv::ExecutionModelTessellationEvaluation;
                    return true;
                case Formats::ShaderStage::Fragment:
                    model = spv::ExecutionModelFragment;
                    return true;
                case Formats::ShaderStage::Compute:
                    model = spv::ExecutionModelGLCompute;
                    return true;
                default:
                    return false;
            }
        }

        // The descriptor count of a resource's type: the product of its array dimensions, kDxilUnboundedCount when
        // the outermost is runtime-sized. A dimension sized by a specialization constant reads its default value.
        uint32_t DxilDescriptorCount( const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& type )
        {
            uint32_t count = 1;
            for ( size_t i = 0; i < type.array.size(); ++i )
            {
                uint32_t size = type.array[i];
                if ( !type.array_size_literal[i] )
                    size = compiler.get_constant( size ).scalar();
                if ( size == 0 )
                    return kDxilUnboundedCount;
                count *= size;
            }
            return count;
        }

        // The stage's resources by class (the register rule's class column), folded into the program's slots.
        // Returns the refusal text, empty when every slot agrees with what another stage declared there.
        std::string DxilCollectSlots( const spirv_cross::Compiler&        compiler,
                                      const spirv_cross::ShaderResources& res, const Formats::ShaderStage stage,
                                      std::map<uint64_t, DxilSlot>& slots )
        {
            const std::string stageName = Graphic::Shader::GetStringShaderStage( stage );
            std::string       refusal;
            const auto        add = [&]( const spirv_cross::SmallVector<spirv_cross::Resource>& list,
                                  const DxilRangeType type, const bool paired )
            {
                for ( const spirv_cross::Resource& resource : list )
                {
                    const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
                    const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
                    const uint32_t count = DxilDescriptorCount( compiler, compiler.get_type( resource.type_id ) );
                    const uint64_t key   = ( static_cast<uint64_t>( set ) << 32 ) | binding;
                    auto [it, inserted]  = slots.try_emplace( key );
                    DxilSlot& slot       = it->second;
                    if ( inserted )
                    {
                        slot = { set, binding, type, paired, count, 0, resource.name, stageName };
                    }
                    else if ( slot.Type != type || slot.Paired != paired || slot.Count != count )
                    {
                        if ( refusal.empty() )
                            refusal = std::format( "set {} binding {}: '{}' in {} and '{}' in {} are different "
                                                   "descriptor kinds or counts on one slot",
                                                   set, binding, slot.Name, slot.DeclaringStage, resource.name,
                                                   stageName );
                    }
                    slot.Stages |= static_cast<uint32_t>( stage );
                }
            };
            add( res.uniform_buffers, DxilRangeType::Cbv, false );
            add( res.storage_buffers, DxilRangeType::Uav, false ); // force_storage_buffer_as_uav
            add( res.storage_images, DxilRangeType::Uav, false );  // storage texel buffers included
            add( res.sampled_images, DxilRangeType::Srv, true );
            add( res.separate_images, DxilRangeType::Srv, false ); // uniform texel buffers included
            add( res.separate_samplers, DxilRangeType::Sampler, false );
            add( res.acceleration_structures, DxilRangeType::Srv, false );
            add( res.subpass_inputs, DxilRangeType::Srv, false );
            return refusal;
        }

        // The register rule's tables from the program's slots (RootSignatureDesc's parameter order, after the root
        // constants): per set, the bounded CBV/SRV/UAV ranges, the bounded samplers, then each unbounded range.
        void DxilAppendTables( const std::map<uint64_t, DxilSlot>& slots, RootSignatureDesc& desc )
        {
            std::map<uint32_t, std::vector<const DxilSlot*>>
                 bySet; // ascending set, then binding (the map's order)
            for ( const auto& [key, slot] : slots )
                bySet[slot.Set].push_back( &slot );

            for ( const auto& [set, list] : bySet )
            {
                DxilRootParameter resources{ DxilRootSource::DescriptorTable };
                DxilRootParameter samplers{ DxilRootSource::DescriptorTable };
                resources.Set                                 = set;
                samplers.Set                                  = set;
                samplers.SamplerTable                         = true;
                uint32_t                       resourceStages = 0;
                uint32_t                       samplerStages  = 0;
                std::vector<DxilRootParameter> unbounded;

                const auto place = [&]( const DxilSlot& slot, const DxilRangeType type )
                {
                    DxilDescriptorRange range{ type, slot.Binding, slot.Set, slot.Count, 0, slot.Name };
                    const bool          isSampler = type == DxilRangeType::Sampler;
                    if ( slot.Count == kDxilUnboundedCount )
                    {
                        DxilRootParameter alone{ DxilRootSource::DescriptorTable };
                        alone.Set          = set;
                        alone.SamplerTable = isSampler;
                        alone.Visibility   = DxilVisibilityOf( slot.Stages );
                        alone.Ranges.push_back( std::move( range ) );
                        unbounded.push_back( std::move( alone ) );
                        return;
                    }
                    DxilRootParameter& table = isSampler ? samplers : resources;
                    uint32_t&          bits  = isSampler ? samplerStages : resourceStages;
                    range.OffsetInTable      = 0;
                    for ( const DxilDescriptorRange& before : table.Ranges )
                        range.OffsetInTable += before.Count;
                    table.Ranges.push_back( std::move( range ) );
                    bits |= slot.Stages;
                };
                for ( const DxilSlot* slot : list )
                {
                    place( *slot, slot->Type );
                    if ( slot->Paired )
                        place( *slot, DxilRangeType::Sampler );
                }
                if ( !resources.Ranges.empty() )
                {
                    resources.Visibility = DxilVisibilityOf( resourceStages );
                    desc.Parameters.push_back( std::move( resources ) );
                }
                if ( !samplers.Ranges.empty() )
                {
                    samplers.Visibility = DxilVisibilityOf( samplerStages );
                    desc.Parameters.push_back( std::move( samplers ) );
                }
                for ( DxilRootParameter& alone : unbounded )
                    desc.Parameters.push_back( std::move( alone ) );
            }
        }

        DxilRootParameter DxilRootConstants( const DxilRootSource source, const uint32_t reg,
                                             const uint32_t values, const uint32_t stageBits )
        {
            DxilRootParameter parameter{ source };
            parameter.Visibility     = DxilVisibilityOf( stageBits );
            parameter.ShaderRegister = reg;
            parameter.RegisterSpace  = kDxilRootConstantSpace;
            parameter.Num32BitValues = values;
            return parameter;
        }

        uint32_t DxilRootSignatureDwords( const RootSignatureDesc& desc )
        {
            uint32_t dwords = 0;
            for ( const DxilRootParameter& parameter : desc.Parameters )
                dwords += parameter.Source == DxilRootSource::DescriptorTable ? 1u : parameter.Num32BitValues;
            return dwords;
        }

        // The vertex stage's input locations; a matrix or array input holds one location per column/element, as
        // flatten_matrix_vertex_input_semantics numbers its TEXCOORDs.
        std::vector<DxilVertexInput> DxilVertexInputsOf( const spirv_cross::Compiler&        compiler,
                                                         const spirv_cross::ShaderResources& res )
        {
            std::vector<DxilVertexInput> inputs;
            for ( const spirv_cross::Resource& input : res.stage_inputs )
            {
                const uint32_t location           = compiler.get_decoration( input.id, spv::DecorationLocation );
                const spirv_cross::SPIRType& type = compiler.get_type( input.type_id );
                uint32_t                     span = type.columns > 1 ? type.columns : 1u;
                for ( const uint32_t size : type.array )
                    span *= size == 0 ? 1u : size;
                for ( uint32_t i = 0; i < span; ++i )
                    inputs.push_back( { location + i } );
            }
            std::sort( inputs.begin(), inputs.end(), []( const DxilVertexInput& a, const DxilVertexInput& b )
                       { return a.Location < b.Location; } );
            return inputs;
        }

        struct DxilStageCompiler
        {
            Formats::ShaderStage                       Stage = Formats::ShaderStage::Vertex;
            spv::ExecutionModel                        Model = spv::ExecutionModelVertex;
            std::unique_ptr<spirv_cross::CompilerHLSL> Compiler;
            spirv_cross::ShaderResources               Resources;
        };

        // The library's own error type is the only thing caught (the one ThirdParty boundary, as in
        // VulkanShaderReflection's ReflectStage): SPIRV-Cross reports SPIR-V it cannot take, and HLSL it cannot
        // express, by throwing spirv_cross::CompilerError. It becomes a refusal of this program, by name.
        Common::ResultStr<DxilHlslProgram> DxilCrossCompile( const std::vector<ShaderMapStage>& stages,
                                                             const std::string_view             shaderName )
        try
        {
            const bool hasTessEvaluation =
                 std::any_of( stages.begin(), stages.end(), []( const ShaderMapStage& s )
                              { return s.Stage == Formats::ShaderStage::TessEvaluation; } );

            std::vector<DxilStageCompiler> compilers;
            compilers.reserve( stages.size() );
            std::map<uint64_t, DxilSlot> slots;
            uint32_t                     pushBytes  = 0;
            uint32_t                     pushStages = 0;
            for ( const ShaderMapStage& stage : stages )
            {
                const std::string   stageName = Graphic::Shader::GetStringShaderStage( stage.Stage );
                spv::ExecutionModel model     = spv::ExecutionModelVertex;
                if ( !DxilExecutionModelOf( stage.Stage, model ) )
                    return Common::MakeFormattedError<DxilHlslProgram>(
                         "shader '{}' [{}]: D3D12 has no DXIL for this stage from SPIR-V (SPIRV-Cross emits no "
                         "HLSL for ray tracing pipeline stages)",
                         shaderName, stageName );
                DxilStageCompiler entry{
                     stage.Stage, model, std::make_unique<spirv_cross::CompilerHLSL>( stage.Spirv ), {} };
                entry.Resources = entry.Compiler->get_shader_resources();
                const std::string conflict =
                     DxilCollectSlots( *entry.Compiler, entry.Resources, stage.Stage, slots );
                if ( !conflict.empty() )
                    return Common::MakeFormattedError<DxilHlslProgram>( "shader '{}': {}", shaderName, conflict );
                for ( const spirv_cross::Resource& block : entry.Resources.push_constant_buffers )
                {
                    const auto size = static_cast<uint32_t>( entry.Compiler->get_declared_struct_size(
                         entry.Compiler->get_type( block.base_type_id ) ) );
                    pushBytes       = std::max( pushBytes, size );
                    pushStages |= static_cast<uint32_t>( stage.Stage );
                }
                compilers.push_back( std::move( entry ) );
            }
            pushBytes = ( pushBytes + 3u ) & ~3u;

            DxilHlslProgram program;
            uint32_t        baseVertexStages = 0;
            uint32_t        workgroupStages  = 0;
            for ( DxilStageCompiler& entry : compilers )
            {
                spirv_cross::CompilerHLSL& compiler = *entry.Compiler;

                spirv_cross::CompilerHLSL::Options hlsl;
                hlsl.shader_model                              = kDxilShaderModel;
                hlsl.support_nonzero_base_vertex_base_instance = true;
                hlsl.force_storage_buffer_as_uav               = true;
                hlsl.enable_16bit_types                        = true;
                hlsl.flatten_matrix_vertex_input_semantics     = true;
                hlsl.point_size_compat                         = true;
                compiler.set_hlsl_options( hlsl );

                // Vulkan's clip space has +Y down the framebuffer, D3D's +Y up the render target: the last stage
                // before the rasterizer negates Position.y, which puts every vertex on the same pixel as on Vulkan
                // (so winding, SV_Position and derivatives match; the runtime keeps Vulkan's front face as is).
                spirv_cross::CompilerGLSL::Options common = compiler.get_common_options();
                common.vertex.flip_vert_y =
                     entry.Stage ==
                     ( hasTessEvaluation ? Formats::ShaderStage::TessEvaluation : Formats::ShaderStage::Vertex );
                common.vertex.fixup_clipspace = false; // both APIs keep depth in [0, 1]
                compiler.set_common_options( common );

                for ( const auto& [key, slot] : slots )
                {
                    if ( ( slot.Stages & static_cast<uint32_t>( entry.Stage ) ) == 0 )
                        continue;
                    spirv_cross::HLSLResourceBinding binding;
                    binding.stage    = entry.Model;
                    binding.desc_set = slot.Set;
                    binding.binding  = slot.Binding;
                    binding.cbv      = { slot.Set, slot.Binding };
                    binding.uav      = { slot.Set, slot.Binding };
                    binding.srv      = { slot.Set, slot.Binding };
                    binding.sampler  = { slot.Set, slot.Binding };
                    compiler.add_hlsl_resource_binding( binding );
                }
                if ( pushBytes > 0 )
                    compiler.set_root_constant_layouts(
                         { { 0u, pushBytes, kDxilPushConstantRegister, kDxilRootConstantSpace } } );
                compiler.set_hlsl_aux_buffer_binding( spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE,
                                                      kDxilBaseVertexInstanceRegister, kDxilRootConstantSpace );
                const spirv_cross::VariableID workgroups = compiler.remap_num_workgroups_builtin();
                if ( workgroups != 0 )
                {
                    compiler.set_decoration( workgroups, spv::DecorationDescriptorSet, kDxilRootConstantSpace );
                    compiler.set_decoration( workgroups, spv::DecorationBinding, kDxilNumWorkgroupsRegister );
                    spirv_cross::HLSLResourceBinding binding;
                    binding.stage    = entry.Model;
                    binding.desc_set = kDxilRootConstantSpace;
                    binding.binding  = kDxilNumWorkgroupsRegister;
                    binding.cbv      = { kDxilRootConstantSpace, kDxilNumWorkgroupsRegister };
                    compiler.add_hlsl_resource_binding( binding );
                    workgroupStages |= static_cast<uint32_t>( entry.Stage );
                }

                DxilHlslStage out{ entry.Stage, compiler.compile(), {} };
                if ( compiler.is_hlsl_aux_buffer_binding_used(
                          spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE ) )
                    baseVertexStages |= static_cast<uint32_t>( entry.Stage );
                if ( entry.Stage == Formats::ShaderStage::Vertex )
                {
                    out.VertexInputs = DxilVertexInputsOf( compiler, entry.Resources );
                    program.RootSignature.AllowInputAssemblerInputLayout = !out.VertexInputs.empty();
                }
                program.Stages.push_back( std::move( out ) );
            }

            RootSignatureDesc& desc = program.RootSignature;
            if ( pushBytes > 0 )
                desc.Parameters.push_back( DxilRootConstants(
                     DxilRootSource::PushConstants, kDxilPushConstantRegister, pushBytes / 4u, pushStages ) );
            if ( baseVertexStages != 0 )
                desc.Parameters.push_back( DxilRootConstants(
                     DxilRootSource::BaseVertexInstance, kDxilBaseVertexInstanceRegister, 2u, baseVertexStages ) );
            if ( workgroupStages != 0 )
                desc.Parameters.push_back( DxilRootConstants( DxilRootSource::NumWorkgroups,
                                                              kDxilNumWorkgroupsRegister, 3u, workgroupStages ) );
            DxilAppendTables( slots, desc );

            const uint32_t dwords = DxilRootSignatureDwords( desc );
            if ( dwords > kDxilRootSignatureDwordLimit )
                return Common::MakeFormattedError<DxilHlslProgram>(
                     "shader '{}': its root signature needs {} DWORDs (push constants "
                     "{} bytes, {} parameters), D3D12 allows {}",
                     shaderName, dwords, pushBytes, desc.Parameters.size(), kDxilRootSignatureDwordLimit );
            return Common::MakeSuccess( std::move( program ) );
        }
        catch ( const spirv_cross::CompilerError& error )
        {
            return Common::MakeFormattedError<DxilHlslProgram>(
                 "shader '{}': SPIRV-Cross cannot express this program as HLSL: {}", shaderName, error.what() );
        }
    } // namespace

    Common::ResultStr<DxilHlslProgram> CrossCompileProgramToHlsl( const std::vector<ShaderMapStage>& stages,
                                                                  const std::string_view             shaderName )
    {
        if ( stages.empty() )
            return Common::MakeFormattedError<DxilHlslProgram>( "shader '{}': no stages to compile to DXIL",
                                                                shaderName );
        return DxilCrossCompile( stages, shaderName );
    }

    Common::ResultStr<RootSignatureDesc> DeriveRootSignatureDXIL( const std::vector<ShaderMapStage>& stages,
                                                                  const std::string_view             shaderName )
    {
        auto program = CrossCompileProgramToHlsl( stages, shaderName );
        if ( !program.IsSuccess() )
            return Common::MakeError<RootSignatureDesc>( program.GetError() );
        return Common::MakeSuccess( std::move( program.ExtractValue().RootSignature ) );
    }

    std::wstring DxilTargetProfile( const Formats::ShaderStage stage )
    {
        const wchar_t* prefix = nullptr;
        switch ( stage )
        {
            case Formats::ShaderStage::Vertex:
                prefix = L"vs";
                break;
            case Formats::ShaderStage::TessControl:
                prefix = L"hs";
                break;
            case Formats::ShaderStage::TessEvaluation:
                prefix = L"ds";
                break;
            case Formats::ShaderStage::Fragment:
                prefix = L"ps";
                break;
            case Formats::ShaderStage::Compute:
                prefix = L"cs";
                break;
            default:
                return {};
        }
        return std::format( L"{}_{}_{}", prefix, kDxilShaderModel / 10, kDxilShaderModel % 10 );
    }
} // namespace Desert::Core

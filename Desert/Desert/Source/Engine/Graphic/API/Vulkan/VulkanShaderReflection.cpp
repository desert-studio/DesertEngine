#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <utility>

namespace Desert::Graphic::API::Vulkan::ShaderReflection
{
    namespace
    {
        // Folds this stage into a resource's stage mask. The entry is default-constructed on first
        // touch, so a resource declared by two stages ends up with both bits and one descriptor.
        template <typename TResource>
        void FillResource( TResource& entry, uint32_t binding, const std::string& name,
                           Core::Formats::ShaderStage stage )
        {
            entry.BindingPoint = binding;
            entry.Name         = name;
            entry.ShaderStage  = ( Core::Formats::ShaderStage )( (uint32_t)entry.ShaderStage | (uint32_t)stage );
        }
    } // namespace

    ImageKind ClassifyImage( const spirv_cross::SPIRType& type )
    {
        const auto& image = type.image;

        // Arrayed and multisampled images need a different view type and a different bind path, and
        // the engine builds neither. Approximating them by their base dimension is what the old name
        // heuristic effectively did — refuse instead.
        if ( image.ms || image.arrayed )
        {
            return ImageKind::Unsupported;
        }

        switch ( image.dim )
        {
            case spv::Dim2D:
                return ImageKind::Image2D;
            case spv::Dim3D:
                return ImageKind::Image3D;
            case spv::DimCube:
                return ImageKind::ImageCube;
            default:
                return ImageKind::Unsupported;
        }
    }

    std::string DescribeImageType( const spirv_cross::SPIRType& type )
    {
        const auto& image = type.image;

        // SPIR-V's `sampled`: 1 = read through a sampler, 2 = read/written as a storage image.
        std::string described = image.sampled == 2 ? "image" : "sampler";

        switch ( image.dim )
        {
            case spv::Dim1D:
                described += "1D";
                break;
            case spv::Dim2D:
                described += "2D";
                break;
            case spv::Dim3D:
                described += "3D";
                break;
            case spv::DimCube:
                described += "Cube";
                break;
            case spv::DimRect:
                described += "2DRect";
                break;
            case spv::DimBuffer:
                described += "Buffer";
                break;
            case spv::DimSubpassData:
                described += "SubpassData";
                break;
            default:
                described += std::format( "<dim {}>", (int)image.dim );
                break;
        }

        if ( image.ms )
        {
            described += "MS";
        }
        if ( image.arrayed )
        {
            described += "Array";
        }

        return described;
    }

    std::vector<std::string> ReflectStage( const std::vector<uint32_t>& spirv, Core::Formats::ShaderStage stage,
                                           ShaderResource::ReflectionData& data )
    {
        std::vector<std::string> diagnostics;

        spirv_cross::CompilerGLSL    compiler( spirv );
        spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        // ONE DESCRIPTOR SLOT, ONE RESOURCE — asserted here because this is the only place that knows
        // every occupied number, whatever spelled it.
        //
        // Nothing upstream can make that guarantee. The DSL's automatic binding allocator seeds itself
        // by scanning ONE TEXT for `binding = <digits>` (DShaderParser.cpp, TranslateLayoutSugar), and it
        // is blind three ways: a binding written as a MACRO is not digits (Common/CloudAuthored.glslh
        // declares its buffer at `binding = CLOUD_AUTHORED_BUFFER_BINDING`), a binding declared in an
        // INCLUDED file is not in the text at all because ShaderIncluder hands every `.glslh` its own
        // separate translation call, and a binding a SECOND STAGE declares is not in this text either.
        // Widening that scan would close the first only, and would still be a race against whatever
        // syntax arrives next.
        // glslang does not close it either: two resources decorated with the same Binding compile clean,
        // with no diagnostic, `-Werror` included (measured with glslc 2026-09-08).
        //
        // And the collision is INVISIBLE further down: the buckets below are keyed BY BINDING, so the
        // second resource overwrites the first, BuildLayoutBindings emits one entry, and the layout this
        // reflection produces is complete, plausible and wrong. Refusing by name beats that (Ф4) —
        // VulkanShader::Reflect logs every message here and drops the shader.
        //
        // The occupancy starts from what EARLIER STAGES already put in `data`, because the same collision
        // exists across a stage boundary: a vertex and a fragment stage naming two DIFFERENT resources on
        // one slot merge into one bucket entry with exactly the same loss. Identity is the resource NAME —
        // one resource declared by two stages is the merge this reflection is built to do (and is what
        // FillResource's stage mask exists for), two names on one slot is the defect.
        std::map<std::pair<uint32_t, uint32_t>, std::string> claimed;
        for ( const auto& [set, descriptorSet] : data.ShaderDescriptorSets )
        {
            const auto remember = [&claimed, set = set]( const auto& bucket )
            {
                for ( const auto& [binding, resource] : bucket )
                    claimed.emplace( std::pair{ set, binding }, resource.Name );
            };
            remember( descriptorSet.UniformBuffers );
            remember( descriptorSet.Image2DSamplers );
            remember( descriptorSet.Image3DSamplers );
            remember( descriptorSet.ImageCubeSamplers );
            remember( descriptorSet.StorageBuffers );
            remember( descriptorSet.StorageImage2DSamplers );
            remember( descriptorSet.StorageImage3DSamplers );
            remember( descriptorSet.AccelerationStructures );
        }

        const auto claimSlot = [&]( const spirv_cross::Resource& resource )
        {
            const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );

            const auto [it, inserted] = claimed.emplace( std::pair{ set, binding }, resource.name );
            if ( !inserted && it->second != resource.name )
            {
                diagnostics.push_back( std::format(
                     "set {}, binding {} is claimed by two resources: '{}' and '{}'; a descriptor slot holds "
                     "one resource, so one of the two would be silently dropped. If either number was "
                     "allocated automatically by the shader DSL, give that declaration an explicit binding",
                     set, binding, it->second, resource.name ) );
            }
        };
        // Every category that consumes a descriptor slot, in the order the buckets below read them, so a
        // shader with two collisions reports them the same way twice.
        for ( const auto& resource : resources.uniform_buffers )
            claimSlot( resource );
        for ( const auto& resource : resources.sampled_images )
            claimSlot( resource );
        for ( const auto& resource : resources.storage_buffers )
            claimSlot( resource );
        for ( const auto& resource : resources.storage_images )
            claimSlot( resource );
        for ( const auto& resource : resources.acceleration_structures )
            claimSlot( resource );

        // Uniform Buffers
        for ( const auto& resource : resources.uniform_buffers )
        {
            uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
            auto&    ub      = data.ShaderDescriptorSets[set].UniformBuffers[binding];
            FillResource( ub, binding, resource.name, stage );

            const auto& structType = compiler.get_type( resource.base_type_id );
            ub.Size                = (uint32_t)compiler.get_declared_struct_size( structType );

            // Populate fields once; multi-stage shaders call ReflectStage() per stage, avoid duplicates.
            if ( ub.Fields.empty() )
            {
                for ( uint32_t i = 0; i < static_cast<uint32_t>( structType.member_types.size() ); ++i )
                {
                    ShaderResources::ShaderLayout::ShaderFieldLayout field;
                    field.Name   = compiler.get_member_name( resource.base_type_id, i );
                    field.Offset = compiler.type_struct_member_offset( structType, i );
                    field.Size   = (uint32_t)compiler.get_declared_struct_member_size( structType, i );

                    const auto& memberType = compiler.get_type( structType.member_types[i] );
                    field.ArraySize        = memberType.array.empty() ? 1u : memberType.array[0];

                    ub.Fields.push_back( std::move( field ) );
                }
            }
        }

        // Sampled images — one bucket per view type, decided by the DECLARED type. The name is never
        // consulted: `u_EnvNoise` may well be a sampler2D and `u_Radiance` a samplerCube.
        for ( const auto& resource : resources.sampled_images )
        {
            uint32_t    set       = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            uint32_t    binding   = compiler.get_decoration( resource.id, spv::DecorationBinding );
            const auto& imageType = compiler.get_type( resource.base_type_id );

            // An ARRAY of samplers (`sampler2D u_Foo[4]`) is a different thing from an arrayed image:
            // it needs descriptorCount > 1, and every layout this engine builds hardcodes 1.
            if ( !compiler.get_type( resource.type_id ).array.empty() )
            {
                diagnostics.push_back( std::format(
                     "resource '{}' (set {}, binding {}) is an array of {}; arrays of descriptors are "
                     "not supported — declare separate bindings",
                     resource.name, set, binding, DescribeImageType( imageType ) ) );
                continue;
            }

            switch ( ClassifyImage( imageType ) )
            {
                case ImageKind::Image2D:
                    FillResource( data.ShaderDescriptorSets[set].Image2DSamplers[binding], binding, resource.name,
                                  stage );
                    break;
                case ImageKind::Image3D:
                    FillResource( data.ShaderDescriptorSets[set].Image3DSamplers[binding], binding, resource.name,
                                  stage );
                    break;
                case ImageKind::ImageCube:
                    FillResource( data.ShaderDescriptorSets[set].ImageCubeSamplers[binding], binding,
                                  resource.name, stage );
                    break;
                case ImageKind::Unsupported:
                    diagnostics.push_back(
                         std::format( "sampled image '{}' (set {}, binding {}) is a {}, which the engine "
                                      "cannot bind",
                                      resource.name, set, binding, DescribeImageType( imageType ) ) );
                    break;
            }
        }

        // Storage Buffers
        for ( const auto& resource : resources.storage_buffers )
        {
            uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
            auto&    sb      = data.ShaderDescriptorSets[set].StorageBuffers[binding];
            FillResource( sb, binding, resource.name, stage );
            auto& type = compiler.get_type( resource.base_type_id );
            sb.Size    = ( type.member_types.empty() || type.array.size() > 0 )
                              ? 0
                              : (uint32_t)compiler.get_declared_struct_size( type );
        }

        // Storage images (`writeonly image2D` / `imageCube` / `image3D` in compute shaders). 2D and
        // cube share a bucket on purpose: a storage binding is written from a mip VIEW the caller
        // supplies, which already carries its own view type, so nothing downstream needs the two
        // apart — and keeping them together is what leaves the existing IBL compute chain untouched
        // by this change. A volume is split off because the storage path binds 2D and cube only.
        for ( const auto& resource : resources.storage_images )
        {
            uint32_t    set       = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            uint32_t    binding   = compiler.get_decoration( resource.id, spv::DecorationBinding );
            const auto& imageType = compiler.get_type( resource.base_type_id );

            if ( !compiler.get_type( resource.type_id ).array.empty() )
            {
                diagnostics.push_back( std::format(
                     "resource '{}' (set {}, binding {}) is an array of {}; arrays of descriptors are "
                     "not supported — declare separate bindings",
                     resource.name, set, binding, DescribeImageType( imageType ) ) );
                continue;
            }

            switch ( ClassifyImage( imageType ) )
            {
                case ImageKind::Image2D:
                case ImageKind::ImageCube:
                    FillResource( data.ShaderDescriptorSets[set].StorageImage2DSamplers[binding], binding,
                                  resource.name, stage );
                    break;
                case ImageKind::Image3D:
                    FillResource( data.ShaderDescriptorSets[set].StorageImage3DSamplers[binding], binding,
                                  resource.name, stage );
                    break;
                case ImageKind::Unsupported:
                    diagnostics.push_back(
                         std::format( "storage image '{}' (set {}, binding {}) is a {}, which the engine "
                                      "cannot bind",
                                      resource.name, set, binding, DescribeImageType( imageType ) ) );
                    break;
            }
        }

        // Acceleration structures (GL_EXT_ray_query). An array of them is refused like an array of images:
        // the layout below gives every binding a descriptorCount of 1.
        for ( const auto& resource : resources.acceleration_structures )
        {
            const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
            if ( !compiler.get_type( resource.type_id ).array.empty() )
            {
                diagnostics.push_back( std::format( "resource '{}' (set {}, binding {}) is an array of "
                                                    "accelerationStructureEXT; arrays of descriptors are not "
                                                    "supported — declare separate bindings",
                                                    resource.name, set, binding ) );
                continue;
            }
            auto& as         = data.ShaderDescriptorSets[set].AccelerationStructures[binding];
            as.DescriptorSet = set;
            FillResource( as, binding, resource.name, stage );
        }

        // Push Constants
        if ( !resources.push_constant_buffers.empty() )
        {
            const auto&    res          = resources.push_constant_buffers[0];
            auto&          type         = compiler.get_type( res.base_type_id );
            const auto     declaredSize = static_cast<uint32_t>( compiler.get_declared_struct_size( type ) );
            if ( !data.PushConstantRanges )
            {
                ShaderResources::ShaderLayout::PushConstantRange range;
                range.Offset            = 0;
                range.Size              = declaredSize;
                range.Name              = res.name;
                range.ShaderStage       = stage;
                data.PushConstantRanges = range;
            }
            else
            {
                // One pipeline, one block: the SIZE is not merged here. ReconcileCellLayout refuses a cell
                // whose stages declare different blocks, and VulkanShader::BuildFromSpirv sets the range to
                // the reconciled MaterialLayout::PushSize — there is no "the larger wins" to hide a stage
                // that declared a shorter block (the T1b 72-vs-68 regression).
                data.PushConstantRanges->ShaderStage = ( Core::Formats::ShaderStage )(
                     (uint32_t)data.PushConstantRanges->ShaderStage | (uint32_t)stage );
            }
        }

        if ( stage == Core::Formats::ShaderStage::Vertex )
            data.VertexInputLocations = ReflectVertexInputLocations( spirv );

        return diagnostics;
    }

    std::vector<uint32_t> ReflectVertexInputLocations( const std::vector<uint32_t>& spirv )
    {
        spirv_cross::Compiler              compiler( spirv );
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        std::vector<uint32_t> locations;
        for ( const auto& input : resources.stage_inputs )
        {
            const uint32_t first = compiler.get_decoration( input.id, spv::DecorationLocation );
            const auto&    type  = compiler.get_type( input.type_id );
            uint32_t       count = std::max( type.columns, 1u );
            for ( const uint32_t length : type.array )
                count *= std::max( length, 1u );
            for ( uint32_t i = 0; i < count; ++i )
                locations.push_back( first + i );
        }
        std::sort( locations.begin(), locations.end() );
        locations.erase( std::unique( locations.begin(), locations.end() ), locations.end() );
        return locations;
    }

    VkFormat VertexAttributeFormat( const ShaderDataType type )
    {
        switch ( type )
        {
            case ShaderDataType::Float:
                return VK_FORMAT_R32_SFLOAT;
            case ShaderDataType::Float2:
                return VK_FORMAT_R32G32_SFLOAT;
            case ShaderDataType::Float3:
                return VK_FORMAT_R32G32B32_SFLOAT;
            case ShaderDataType::Float4:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case ShaderDataType::Int:
                return VK_FORMAT_R32_SINT;
            case ShaderDataType::Int2:
                return VK_FORMAT_R32G32_SINT;
            case ShaderDataType::Int3:
                return VK_FORMAT_R32G32B32_SINT;
            case ShaderDataType::Int4:
                return VK_FORMAT_R32G32B32A32_SINT;
            case ShaderDataType::Bool:
                return VK_FORMAT_R8_UINT;
            case ShaderDataType::UNorm8x4:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case ShaderDataType::None:
                break;
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool VertexInputState::HasBinding( const uint32_t binding ) const
    {
        return std::ranges::any_of( Bindings, [binding]( const VkVertexInputBindingDescription& b )
                                    { return b.binding == binding; } );
    }

    bool VertexInputState::HasLocation( const uint32_t location ) const
    {
        return std::ranges::any_of( Attributes, [location]( const VkVertexInputAttributeDescription& a )
                                    { return a.location == location; } );
    }

    VertexInputState BuildVertexInput( const VertexBufferLayout&    layout,
                                       const std::vector<uint32_t>& consumedLocations )
    {
        VertexInputState state;
        const auto       consumed = [&consumedLocations]( const uint32_t location )
        { return std::ranges::binary_search( consumedLocations, location ); };

        // Every location the layout feeds, whether or not the stage reads it: what is left of
        // consumedLocations after this is input the stage reads and no stream provides.
        std::vector<uint32_t> fed;
        const auto            addAttribute =
             [&]( const VertexBufferElement& element, const uint32_t location, const uint32_t binding )
        {
            fed.push_back( location );
            if ( !consumed( location ) )
                return false;
            const VkFormat format = VertexAttributeFormat( element.Type );
            if ( format == VK_FORMAT_UNDEFINED )
            {
                state.Errors.push_back( std::format( "vertex attribute '{}' (location {}) has no Vulkan format",
                                                     element.Name, location ) );
                return false;
            }
            state.Attributes.push_back(
                 { .location = location, .binding = binding, .format = format, .offset = element.Offset } );
            return true;
        };

        state.Bindings.push_back(
             { .binding = 0, .stride = layout.GetStride(), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX } );
        for ( uint32_t location = 0; const auto& element : layout )
            addAttribute( element, location++, 0 );

        // Binding 1 (VertexBufferLayout::WithStreams) only when the stage reads at least one stream.
        bool streamRead = false;
        for ( uint32_t    location = layout.GetStreamFirstLocation();
              const auto& element : layout.GetStreamElements() )
            streamRead = addAttribute( element, location++, 1 ) || streamRead;
        if ( streamRead )
            state.Bindings.push_back(
                 { .binding = 1, .stride = layout.GetStreamStride(), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX } );

        for ( const uint32_t location : consumedLocations )
        {
            if ( std::ranges::find( fed, location ) == fed.end() )
                state.Errors.push_back( std::format(
                     "the vertex stage reads location {}, which the vertex layout does not feed", location ) );
        }
        return state;
    }

    std::vector<VkDescriptorSetLayoutBinding> BuildLayoutBindings( const ShaderResource::ShaderDescriptorSet& set )
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;

        const auto add = [&bindings]( uint32_t binding, VkDescriptorType type, Core::Formats::ShaderStage stage )
        {
            bindings.push_back( { .binding         = binding,
                                  .descriptorType  = type,
                                  .descriptorCount = 1,
                                  .stageFlags      = static_cast<VkShaderStageFlags>( stage ) } );
        };

        for ( const auto& [binding, resource] : set.UniformBuffers )
            add( binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.Image2DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.Image3DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.ImageCubeSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.StorageBuffers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.StorageImage2DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.StorageImage3DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.AccelerationStructures )
            add( binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, resource.ShaderStage );

        // Sorted because the buckets above are unordered_maps: the same shader would otherwise produce
        // the same SET of bindings in a different ORDER from run to run. Vulkan does not care, but a
        // human comparing two logs of a mismatch does, and so does a test that prints what it found.
        std::sort( bindings.begin(), bindings.end(),
                   []( const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b )
                   { return a.binding < b.binding; } );

        return bindings;
    }

    uint32_t CountDescriptors( const std::vector<VkDescriptorSetLayoutBinding>& bindings )
    {
        uint32_t total = 0;
        for ( const auto& binding : bindings )
            total += binding.descriptorCount;
        return total;
    }

    Core::Formats::ReflectedMaterialStage ReflectMaterialStage( const std::vector<uint32_t>& spirv,
                                                                Core::Formats::ShaderStage   stage )
    {
        Core::Formats::ReflectedMaterialStage out;
        out.Stage = stage;

        spirv_cross::Compiler compiler( spirv );
        const auto            resources = compiler.get_shader_resources();

        const auto membersOf = [&]( const spirv_cross::SPIRType& structType, spirv_cross::TypeID typeId )
        {
            std::vector<Core::Formats::ReflectedLayoutMember> members;
            for ( uint32_t i = 0; i < static_cast<uint32_t>( structType.member_types.size() ); ++i )
                members.push_back(
                     { compiler.get_member_name( typeId, i ), compiler.type_struct_member_offset( structType, i ),
                       static_cast<uint32_t>( compiler.get_declared_struct_member_size( structType, i ) ) } );
            return members;
        };

        for ( const auto& resource : resources.storage_buffers )
        {
            const auto& block = compiler.get_type( resource.base_type_id );
            if ( compiler.get_name( resource.base_type_id ) != Core::Formats::kMaterialRowBlockName &&
                 resource.name != Core::Formats::kMaterialRowBlockName )
                continue;
            if ( block.member_types.empty() )
                continue;
            out.RowBinding    = compiler.get_decoration( resource.id, spv::DecorationBinding );
            const auto& array = compiler.get_type( block.member_types[0] );
            out.RowStride     = compiler.type_struct_member_array_stride( block, 0 );
            // A runtime array's element type is its parent; the member names live on that struct type.
            const spirv_cross::TypeID rowId =
                 array.array.empty() ? spirv_cross::TypeID( array.self ) : array.parent_type;
            const auto& row = compiler.get_type( rowId );
            if ( row.basetype == spirv_cross::SPIRType::Struct )
                out.RowMembers = membersOf( row, rowId );
        }

        for ( const auto& resource : resources.sampled_images )
            out.Samplers.push_back(
                 { resource.name, compiler.get_decoration( resource.id, spv::DecorationBinding ), 0 } );

        for ( const auto* list :
              { &resources.uniform_buffers, &resources.storage_buffers, &resources.sampled_images } )
            for ( const auto& resource : *list )
                out.ResourceNames.push_back( resource.name );

        if ( !resources.push_constant_buffers.empty() )
        {
            const auto& res  = resources.push_constant_buffers[0];
            const auto& type = compiler.get_type( res.base_type_id );
            out.PushSize     = static_cast<uint32_t>( compiler.get_declared_struct_size( type ) );
            out.PushMembers  = membersOf( type, res.base_type_id );
        }
        return out;
    }

    ReconciledCellLayout ReconcileCellLayout( const Core::Formats::ShaderProgramMeta&  meta,
                                              const std::vector<Core::ShaderMapStage>& stages,
                                              std::string_view templateName, std::string_view cellName )
    {
        ReconciledCellLayout                               out{ Core::Formats::BuildMaterialLayout( meta ), {} };
        std::vector<Core::Formats::ReflectedMaterialStage> reflected;
        reflected.reserve( stages.size() );
        for ( const auto& stage : stages )
            reflected.push_back( ReflectMaterialStage( stage.Spirv, stage.Stage ) );
        out.Errors = Core::Formats::ReconcileMaterialLayout( out.Layout, templateName, cellName, reflected );
        for ( const auto& stage : reflected )
            out.Layout.SceneReads = out.Layout.SceneReads | SceneResources::Classify( stage.ResourceNames );
        return out;
    }

} // namespace Desert::Graphic::API::Vulkan::ShaderReflection

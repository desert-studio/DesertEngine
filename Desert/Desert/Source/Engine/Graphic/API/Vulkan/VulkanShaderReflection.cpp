#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanBindlessTextureTable.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Core/ShaderCompiler/SharedSamplerBinding.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
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

    namespace
    {
        ShaderResources::ShaderLayout::SampledImageDimension SampledDimensionOf( ImageKind kind, bool arrayed )
        {
            using Dimension = ShaderResources::ShaderLayout::SampledImageDimension;
            switch ( kind )
            {
                case ImageKind::Image2DMultisample:
                    return Dimension::Texture2DMultisample;
                case ImageKind::Image3D:
                    return Dimension::Texture3D;
                case ImageKind::ImageCube:
                    return arrayed ? Dimension::CubeArray : Dimension::Cube;
                case ImageKind::Image2D:
                    return arrayed ? Dimension::Texture2DArray : Dimension::Texture2D;
                case ImageKind::Unsupported:
                    break;
            }
            return Dimension::Texture2D;
        }
    } // namespace

    ImageKind ClassifyImage( const spirv_cross::SPIRType& type )
    {
        const auto& image = type.image;

        // Arrayed images need a different view type and a different bind path. The ones the engine builds are
        // the CUBE ARRAY and the 2D ARRAY, and only through the render graph: an RDG::TextureDim::CubeArray /
        // Tex2DArray texture's views are CUBE_ARRAY / 2D_ARRAY views at every layer count (the reflection
        // captures, samplerCubeArray / imageCubeArray; the sun's shadow cascades, sampler2DArray), so each
        // shares its base dimension's bucket - the view, not the bucket, carries the type. Every other arrayed
        // image is refused rather than approximated by its base dimension.
        if ( image.arrayed )
        {
            if ( image.dim == spv::DimCube && !image.ms )
                return ImageKind::ImageCube;
            if ( image.dim == spv::Dim2D && !image.ms )
                return ImageKind::Image2D;
            return ImageKind::Unsupported;
        }
        // A multisampled image is only ever 2D (Vulkan has no 3D or cube MS image); its view is a plain 2D
        // view and its descriptor a combined image sampler, exactly as for sampler2D.
        if ( image.ms )
        {
            return image.dim == spv::Dim2D ? ImageKind::Image2DMultisample : ImageKind::Unsupported;
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
            remember( descriptorSet.StorageBuffers );
            remember( descriptorSet.StorageImage2DSamplers );
            remember( descriptorSet.StorageImage3DSamplers );
            remember( descriptorSet.AccelerationStructures );
            remember( descriptorSet.SampledImages );
            remember( descriptorSet.Samplers );
        }

        const auto claimSlot = [&]( const spirv_cross::Resource& resource )
        {
            const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );

            // The bindless set belongs to the global table (BindlessTextures.glslh): its layout is the table's,
            // never one built from this shader, so a resource of the shader's own there has no descriptor.
            if ( set == kBindlessSetIndex )
            {
                diagnostics.push_back( std::format(
                     "resource '{}' (set {}, binding {}) is in the set reserved for the bindless texture table; "
                     "only Common/BindlessTextures.glslh declares set {} — move the resource to another set",
                     resource.name, set, binding, kBindlessSetIndex ) );
                return;
            }
            // The shared-sampler marker is renumbered by the program build (ShaderMapBuild.cpp
            // AssignSharedSamplerBindings) before any reflection: SPIR-V that still carries it did not come
            // through that build, and its sampler has no slot of its own.
            if ( binding == Core::kAutoSamplerBinding )
            {
                diagnostics.push_back( std::format(
                     "resource '{}' (set {}) is still on the shared-sampler marker binding {}: the SPIR-V did not "
                     "come through the program build that numbers `Uniform sampler` declarations (BuildShaderMap)",
                     resource.name, set, binding ) );
                return;
            }
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

        // Combined image samplers are refused: every texture is a separate texture read through a separate sampler
        // state (a pass's shared states, a material property's own `<name>Sampler`, the bindless table's array).
        for ( const auto& resource : resources.sampled_images )
        {
            diagnostics.push_back( std::format(
                 "resource '{}' (set {}, binding {}) is a combined image sampler ({}); declare a separate texture "
                 "(texture2D / textureCube / texture3D) and read it through a `sampler` state",
                 resource.name, compiler.get_decoration( resource.id, spv::DecorationDescriptorSet ),
                 compiler.get_decoration( resource.id, spv::DecorationBinding ),
                 DescribeImageType( compiler.get_type( resource.base_type_id ) ) ) );
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
                // A multisampled storage image (image2DMS) would be written through a mip view of a
                // single-sample image, which is all the storage path builds: refused like any other.
                case ImageKind::Image2DMultisample:
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

        // SEPARATE SAMPLERS AND TEXTURES. In the bindless set they exist only as Common/BindlessTextures.glslh
        // declares them — u_BindlessSamplers[kBindlessSamplerStateCount] at kBindlessSamplerBinding and the
        // runtime array u_BindlessTextures[] at kBindlessTextureBinding. Recognised by set, binding and type (not
        // by name); VulkanShader then takes the table's own layout for that set, and anything else there is
        // refused by name. In every other set they are a pass's own inputs, UE's way: a `texture2D` (or a fixed
        // array of them, textureCube, textureCubeArray) is a SAMPLED_IMAGE slot and a `sampler` a SAMPLER slot,
        // both bound by the pass's RDG bindings (RDG::ShaderResourceKind::Texture / Sampler). A few shared sampler
        // states then serve many inputs, which is what keeps a screen pass under maxPerStageDescriptorSamplers.
        const auto refuseBindless = [&]( const spirv_cross::Resource& resource, std::string_view expected )
        {
            diagnostics.push_back( std::format(
                 "resource '{}' (set {}, binding {}) is a separate sampler/texture in the bindless table's set, "
                 "which only the table declares ({}); include Common/BindlessTextures.glslh instead",
                 resource.name, compiler.get_decoration( resource.id, spv::DecorationDescriptorSet ),
                 compiler.get_decoration( resource.id, spv::DecorationBinding ), expected ) );
        };
        const auto isBindlessSlot = [&]( const spirv_cross::Resource& resource, uint32_t binding )
        {
            return compiler.get_decoration( resource.id, spv::DecorationDescriptorSet ) == kBindlessSetIndex &&
                   compiler.get_decoration( resource.id, spv::DecorationBinding ) == binding;
        };
        const auto inBindlessSet = [&]( const spirv_cross::Resource& resource )
        { return compiler.get_decoration( resource.id, spv::DecorationDescriptorSet ) == kBindlessSetIndex; };
        const auto useTable = [&]()
        {
            data.UsesBindlessTable = true;
            data.BindlessStages =
                 ( Core::Formats::ShaderStage )( (uint32_t)data.BindlessStages | (uint32_t)stage );
        };
        for ( const auto& resource : resources.separate_samplers )
        {
            const auto&    type    = compiler.get_type( resource.type_id );
            const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
            if ( !inBindlessSet( resource ) )
            {
                claimSlot( resource );
                if ( !type.array.empty() )
                {
                    diagnostics.push_back( std::format( "resource '{}' (set {}, binding {}) is an array of "
                                                        "samplers; a pass's sampler slot is one "
                                                        "shared state - declare one `sampler` per state",
                                                        resource.name, set, binding ) );
                    continue;
                }
                auto& sampler         = data.ShaderDescriptorSets[set].Samplers[binding];
                sampler.DescriptorSet = set;
                FillResource( sampler, binding, resource.name, stage );
                continue;
            }
            if ( !isBindlessSlot( resource, kBindlessSamplerBinding ) || type.array.size() != 1 ||
                 !type.array_size_literal[0] || type.array[0] != kBindlessSamplerStateCount )
            {
                refuseBindless( resource,
                                std::format( "sampler[{}] at set {}, binding {}", kBindlessSamplerStateCount,
                                             kBindlessSetIndex, kBindlessSamplerBinding ) );
                continue;
            }
            useTable();
        }
        for ( const auto& resource : resources.separate_images )
        {
            const auto&    type    = compiler.get_type( resource.type_id );
            const uint32_t set     = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
            const uint32_t binding = compiler.get_decoration( resource.id, spv::DecorationBinding );
            if ( !inBindlessSet( resource ) )
            {
                claimSlot( resource );
                // A fixed array is ArraySize descriptors of one binding (dstArrayElement per element); a runtime
                // array has no count a pass could fill, and more than one dimension no element name. Every image
                // dimension a pass reads is accepted (2D, 3D, cube, cube array, multisampled 2D): a SAMPLED_IMAGE
                // descriptor carries the view, and the view carries the dimension.
                const bool fixedArray = type.array.size() == 1 && type.array_size_literal[0] && type.array[0] > 0;
                const ImageKind kind  = ClassifyImage( type );
                if ( ( !type.array.empty() && !fixedArray ) || kind == ImageKind::Unsupported )
                {
                    diagnostics.push_back( std::format(
                         "resource '{}' (set {}, binding {}) is a separate {}{}; a pass's separate texture is a "
                         "texture2D, texture2DMS, texture3D, textureCube or textureCubeArray, alone or in a "
                         "fixed-size array",
                         resource.name, set, binding, DescribeImageType( type ),
                         type.array.empty() ? "" : " array" ) );
                    continue;
                }
                auto& image         = data.ShaderDescriptorSets[set].SampledImages[binding];
                image.DescriptorSet = set;
                image.ArraySize     = fixedArray ? type.array[0] : 1u;
                image.Dimension     = SampledDimensionOf( kind, type.image.arrayed );
                FillResource( image, binding, resource.name, stage );
                continue;
            }
            // A runtime array (`[]`) reflects as one literal dimension of 0.
            if ( !isBindlessSlot( resource, kBindlessTextureBinding ) || type.image.dim != spv::Dim2D ||
                 type.image.arrayed || type.image.ms || type.array.size() != 1 || !type.array_size_literal[0] ||
                 type.array[0] != 0 )
            {
                refuseBindless( resource, std::format( "texture2D[] at set {}, binding {}", kBindlessSetIndex,
                                                       kBindlessTextureBinding ) );
                continue;
            }
            useTable();
        }

        // Push Constants
        if ( !resources.push_constant_buffers.empty() )
        {
            const auto&    res          = resources.push_constant_buffers[0];
            auto&          type         = compiler.get_type( res.base_type_id );
            const auto     declaredSize = static_cast<uint32_t>( compiler.get_declared_struct_size( type ) );
            // The engine's push-block cap (ShaderReflectionTypes.hpp): a block past it is refused here, at
            // compile time, by name and size, so it never reaches a device that may not hold it.
            if ( declaredSize > ShaderResources::ShaderLayout::kMaxPushConstantBytes )
            {
                // The block is named by its TYPE: `uniform PastTheCap { ... };` has no instance name, and
                // res.name is the instance name, empty for every anonymous push block.
                diagnostics.push_back( std::format( "push block '{}' is {} bytes; the engine cap is {} bytes",
                                                    compiler.get_name( res.base_type_id ), declaredSize,
                                                    ShaderResources::ShaderLayout::kMaxPushConstantBytes ) );
                return diagnostics;
            }
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
        if ( stage == Core::Formats::ShaderStage::Fragment )
            data.FragmentOutputLocations = ReflectFragmentOutputLocations( spirv );

        return diagnostics;
    }

    std::vector<uint32_t> ReflectVertexInputLocations( const std::vector<uint32_t>& spirv )
    {
        const spirv_cross::Compiler        compiler( spirv );
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

    std::vector<uint32_t> ReflectFragmentOutputLocations( const std::vector<uint32_t>& spirv )
    {
        const spirv_cross::Compiler        compiler( spirv );
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        std::vector<uint32_t> locations;
        for ( const auto& output : resources.stage_outputs )
        {
            const uint32_t first = compiler.get_decoration( output.id, spv::DecorationLocation );
            const auto&    type  = compiler.get_type( output.type_id );
            uint32_t       count = 1;
            for ( const uint32_t length : type.array )
                count *= std::max( length, 1u );
            for ( uint32_t i = 0; i < count; ++i )
                locations.push_back( first + i );
        }
        std::sort( locations.begin(), locations.end() );
        locations.erase( std::unique( locations.begin(), locations.end() ), locations.end() );
        return locations;
    }

    std::vector<VkPipelineColorBlendAttachmentState>
    BuildColorBlendAttachments( const std::vector<uint32_t>& writtenLocations,
                                const std::vector<bool>& blendPerSlot, const VkBlendFactor srcColor,
                                const VkBlendFactor dstColor )
    {
        const auto colorAttachmentCount = static_cast<uint32_t>( blendPerSlot.size() );
        std::vector<VkPipelineColorBlendAttachmentState> attachments( colorAttachmentCount );
        for ( uint32_t slot = 0; slot < colorAttachmentCount; ++slot )
        {
            const bool written = std::binary_search( writtenLocations.begin(), writtenLocations.end(), slot );
            attachments[slot]  = { .blendEnable         = ( written && blendPerSlot[slot] ) ? VK_TRUE : VK_FALSE,
                                   .srcColorBlendFactor = srcColor,
                                   .dstColorBlendFactor = dstColor,
                                   .colorBlendOp        = VK_BLEND_OP_ADD,
                                   .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                   .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                   .alphaBlendOp        = VK_BLEND_OP_ADD,
                                   .colorWriteMask      = written
                                                               ? ( VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT )
                                                               : VkColorComponentFlags{ 0 } };
        }
        return attachments;
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

    std::optional<std::string> VertexInputRefusal( const VertexInputState& state )
    {
        if ( state.Errors.empty() )
            return std::nullopt;
        std::string reasons;
        for ( const auto& error : state.Errors )
            std::format_to( std::back_inserter( reasons ), "{}{}", reasons.empty() ? "" : "; ", error );
        return reasons;
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
        for ( const auto& [binding, resource] : set.StorageBuffers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.StorageImage2DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.StorageImage3DSamplers )
            add( binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.AccelerationStructures )
            add( binding, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, resource.ShaderStage );
        for ( const auto& [binding, resource] : set.SampledImages )
        {
            add( binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, resource.ShaderStage );
            bindings.back().descriptorCount = resource.ArraySize;
        }
        for ( const auto& [binding, resource] : set.Samplers )
            add( binding, VK_DESCRIPTOR_TYPE_SAMPLER, resource.ShaderStage );

        // Sorted because the buckets above are unordered_maps: the same shader would otherwise produce
        // the same SET of bindings in a different ORDER from run to run. Vulkan does not care, but a
        // human comparing two logs of a mismatch does, and so does a test that prints what it found.
        std::sort( bindings.begin(), bindings.end(),
                   []( const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b )
                   { return a.binding < b.binding; } );

        return bindings;
    }

    namespace
    {
        // One descriptor kind of one stage: how many, and which resources, so a refusal names what to move.
        struct StageTally
        {
            uint32_t    Count = 0;
            std::string Names;

            void Add( std::string_view name, uint32_t descriptors = 1 )
            {
                Count += descriptors;
                std::format_to( std::back_inserter( Names ), "{}{}", Names.empty() ? "" : ", ", name );
            }
        };

        struct StageTallies
        {
            StageTally Samplers;
            StageTally SampledImages;
            StageTally UniformBuffers;
            StageTally StorageBuffers;
            StageTally StorageImages;
        };

        bool InStage( Core::Formats::ShaderStage mask, Core::Formats::ShaderStage stage )
        {
            return ( static_cast<uint32_t>( mask ) & static_cast<uint32_t>( stage ) ) != 0;
        }

        template <typename TBucket>
        void TallyBucket( const TBucket& bucket, Core::Formats::ShaderStage stage, StageTally& first,
                          StageTally* second )
        {
            for ( const auto& [binding, resource] : bucket )
            {
                if ( !InStage( resource.ShaderStage, stage ) )
                    continue;
                // An arrayed binding is ArraySize descriptors against the limit, not one.
                uint32_t descriptors = 1;
                if constexpr ( requires( const typename TBucket::mapped_type& r ) { r.ArraySize; } )
                    descriptors = resource.ArraySize;
                first.Add( resource.Name, descriptors );
                if ( second != nullptr )
                    second->Add( resource.Name, descriptors );
            }
        }

        StageTallies TallyStage( const ShaderResource::ReflectionData& data, Core::Formats::ShaderStage stage )
        {
            StageTallies tallies;
            for ( const auto& [setIndex, set] : data.ShaderDescriptorSets )
            {
                TallyBucket( set.UniformBuffers, stage, tallies.UniformBuffers, nullptr );
                TallyBucket( set.StorageBuffers, stage, tallies.StorageBuffers, nullptr );
                TallyBucket( set.StorageImage2DSamplers, stage, tallies.StorageImages, nullptr );
                TallyBucket( set.StorageImage3DSamplers, stage, tallies.StorageImages, nullptr );
                // A separate texture is sampled images only (ArraySize of them); a sampler slot is a sampler only.
                TallyBucket( set.SampledImages, stage, tallies.SampledImages, nullptr );
                TallyBucket( set.Samplers, stage, tallies.Samplers, nullptr );
            }
            return tallies;
        }

        std::string_view StageName( Core::Formats::ShaderStage stage )
        {
            switch ( stage )
            {
                case Core::Formats::ShaderStage::Vertex:
                    return "vertex";
                case Core::Formats::ShaderStage::TessControl:
                    return "tessellation control";
                case Core::Formats::ShaderStage::TessEvaluation:
                    return "tessellation evaluation";
                case Core::Formats::ShaderStage::Fragment:
                    return "fragment";
                case Core::Formats::ShaderStage::Compute:
                    return "compute";
                case Core::Formats::ShaderStage::None:
                    break;
            }
            return "none";
        }

        void RefuseOver( std::vector<std::string>& refusals, std::string_view stage, const StageTally& tally,
                         uint32_t limit, std::string_view kind, std::string_view limitName )
        {
            if ( tally.Count > limit )
                refusals.push_back( std::format( "[{}] {} {} (device limit {}, {}): {}", stage, tally.Count, kind,
                                                 limit, limitName, tally.Names ) );
        }
    } // namespace

    std::vector<std::string> PerStageLimitRefusals( const ShaderResource::ReflectionData& data,
                                                    const PerStageDescriptorLimits&       limits,
                                                    const DescriptorIndexingLimits&       updateAfterBind )
    {
        using Core::Formats::ShaderStage;
        constexpr std::array kStages{ ShaderStage::Vertex, ShaderStage::TessControl, ShaderStage::TessEvaluation,
                                      ShaderStage::Fragment, ShaderStage::Compute };
        std::vector<std::string> refusals;
        for ( const ShaderStage stage : kStages )
        {
            const StageTallies     tallies   = TallyStage( data, stage );
            const std::string_view stageName = StageName( stage );
            RefuseOver( refusals, stageName, tallies.Samplers, limits.MaxSamplers, "samplers",
                        "maxPerStageDescriptorSamplers" );
            RefuseOver( refusals, stageName, tallies.SampledImages, limits.MaxSampledImages, "sampled images",
                        "maxPerStageDescriptorSampledImages" );
            RefuseOver( refusals, stageName, tallies.UniformBuffers, limits.MaxUniformBuffers, "uniform buffers",
                        "maxPerStageDescriptorUniformBuffers" );
            RefuseOver( refusals, stageName, tallies.StorageBuffers, limits.MaxStorageBuffers, "storage buffers",
                        "maxPerStageDescriptorStorageBuffers" );
            RefuseOver( refusals, stageName, tallies.StorageImages, limits.MaxStorageImages, "storage images",
                        "maxPerStageDescriptorStorageImages" );

            // maxPerStageResources also counts the fragment stage's colour attachments.
            const uint32_t attachments =
                 stage == ShaderStage::Fragment ? static_cast<uint32_t>( data.FragmentOutputLocations.size() ) : 0;
            const uint32_t resources = tallies.SampledImages.Count + tallies.UniformBuffers.Count +
                                       tallies.StorageBuffers.Count + tallies.StorageImages.Count + attachments;
            if ( resources > limits.MaxResources )
                refusals.push_back(
                     std::format( "[{}] {} resources (device limit {}, maxPerStageResources): {} "
                                  "sampled image(s), {} uniform buffer(s), {} storage buffer(s), {} "
                                  "storage image(s), {} colour attachment(s)",
                                  stageName, resources, limits.MaxResources, tallies.SampledImages.Count,
                                  tallies.UniformBuffers.Count, tallies.StorageBuffers.Count,
                                  tallies.StorageImages.Count, attachments ) );

            // The update-after-bind limit counts EVERY sampler the stage sees: its own and the table's.
            if ( InStage( data.BindlessStages, stage ) )
            {
                const uint32_t samplers = tallies.Samplers.Count + kBindlessSamplerStateCount;
                if ( samplers > updateAfterBind.MaxPerStageUpdateAfterBindSamplers )
                    refusals.push_back( std::format( "[{}] {} samplers with the bindless table's {} (device limit "
                                                     "{}, maxPerStageDescriptorUpdateAfterBindSamplers): {}",
                                                     stageName, samplers, kBindlessSamplerStateCount,
                                                     updateAfterBind.MaxPerStageUpdateAfterBindSamplers,
                                                     tallies.Samplers.Names ) );
            }
        }
        return refusals;
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

        // The stage's bound textures (separate textures outside the bindless table's set): where a generated
        // texture property landed, held against the template's layout by ReconcileMaterialLayout.
        for ( const auto& resource : resources.separate_images )
            if ( compiler.get_decoration( resource.id, spv::DecorationDescriptorSet ) != kBindlessSetIndex )
                out.Samplers.push_back(
                     { resource.name, compiler.get_decoration( resource.id, spv::DecorationBinding ), 0 } );

        for ( const auto* list :
              { &resources.uniform_buffers, &resources.storage_buffers, &resources.separate_images } )
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

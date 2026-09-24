#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/VertexBuffer.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        static VkPolygonMode ConvertVkPolygonMode( PrimitivePolygonMode mode )
        {
            switch ( mode )
            {
                case PrimitivePolygonMode::Solid:
                    return VK_POLYGON_MODE_FILL;
                case PrimitivePolygonMode::Wireframe:
                    // LINE requires the fillModeNonSolid device feature — fall back to solid where the
                    // device lacks it (spec violation + validation error otherwise).
                    return EngineContext::GetInstance()
                                     .GetDevice()
                                     ->GetCapabilities()
                                     .SupportsNonSolidFill
                                ? VK_POLYGON_MODE_LINE
                                : VK_POLYGON_MODE_FILL;
            }
            return VK_POLYGON_MODE_FILL;
        }

        static VkStencilOp ConvertStencilOp( StencilOp op )
        {
            switch ( op )
            {
                case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
                case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
                case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
                case StencilOp::IncrementAndClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case StencilOp::DecrementAndClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
                case StencilOp::IncrementAndWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                case StencilOp::DecrementAndWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
                default: return VK_STENCIL_OP_KEEP;
            }
        }

        static VkCompareOp ConvertCompareOp( CompareOp op )
        {
            switch ( op )
            {
                case CompareOp::Never: return VK_COMPARE_OP_NEVER;
                case CompareOp::Less: return VK_COMPARE_OP_LESS;
                case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
                case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
                case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
                case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
                case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
                default: return VK_COMPARE_OP_ALWAYS;
            }
        }

        static VkBlendFactor ConvertBlendFactor( BlendFactor f )
        {
            switch ( f )
            {
                case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
                case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
                case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
                case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
                case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
                case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
                case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                default:                            return VK_BLEND_FACTOR_ONE;
            }
        }

        static VkCullModeFlags ConvertCullMode( CullMode mode )
        {
            switch ( mode )
            {
                case CullMode::None: return VK_CULL_MODE_NONE;
                case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
                case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
                case CullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
                default: return VK_CULL_MODE_BACK_BIT;
            }
        }

        static VkFormat ShaderDataTypeToVulkanFormat( ShaderDataType type )
        {
            switch ( type )
            {
                case ShaderDataType::Float:  return VK_FORMAT_R32_SFLOAT;
                case ShaderDataType::Float2: return VK_FORMAT_R32G32_SFLOAT;
                case ShaderDataType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
                case ShaderDataType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
                case ShaderDataType::Int:    return VK_FORMAT_R32_SINT;
                case ShaderDataType::Int2:   return VK_FORMAT_R32G32_SINT;
                case ShaderDataType::Int3:   return VK_FORMAT_R32G32B32_SINT;
                case ShaderDataType::Int4:   return VK_FORMAT_R32G32B32A32_SINT;
                case ShaderDataType::Bool:   return VK_FORMAT_R8_UINT;
                default:
                {
                    DESERT_VERIFY( false, "Unknown ShaderDataType!" );
                    return VK_FORMAT_UNDEFINED;
                }
            }
        }
    }

    VulkanPipeline::VulkanPipeline( const GraphicsPipelineSpecification& specification ) 
        : m_Specification( specification )
    {
    }

    VulkanPipeline::~VulkanPipeline()
    {
        Release();
    }

    void VulkanPipeline::Release()
    {
        if ( m_Pipeline == VK_NULL_HANDLE && m_PipelineLayout == VK_NULL_HANDLE )
            return;

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();
        if ( m_Pipeline != VK_NULL_HANDLE )
        {
            vkDestroyPipeline( device, m_Pipeline, nullptr );
            m_Pipeline = VK_NULL_HANDLE;
        }
        if ( m_PipelineLayout != VK_NULL_HANDLE )
        {
            vkDestroyPipelineLayout( device, m_PipelineLayout, nullptr );
            m_PipelineLayout = VK_NULL_HANDLE;
        }

        // After the pipeline layout, never before: it was built from these.
        m_Layouts.clear();
    }

    void VulkanPipeline::Invalidate()
    {
        Release();

        // THE DEVICE-FREE HALF OF THE RULE, ASKED AGAIN AT THE ONE PLACE EVERY PIPELINE PASSES THROUGH.
        // GraphicsPipeline::Create asks CheckGraphicsPipelineSpecification before it constructs this
        // object, so in the engine as it stands this arm cannot be reached — it is the last line of
        // defence for a future caller that builds a VulkanPipeline directly, and it is what makes
        // "refuse by leaving the handle null" the leaf's own contract rather than the factory's.
        //
        // `Shader &&` USED TO STAND WHERE `!Shader ||` STANDS NOW, and that single operator was a live
        // null dereference: four renderers put an unchecked ShaderService::GetByName straight into a
        // spec, and a NULL shader passed this guard into CreatePipelineLayout, which calls a method on
        // it. "Has no compiled stages" and "is not there at all" both mean this pipeline cannot exist.
        if ( !m_Specification.Shader || !m_Specification.Shader->IsCompiled() )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: shader '{}' has no compiled stages (see the shader "
                       "compilation error above). The draws using it are skipped.",
                       m_Specification.DebugName,
                       m_Specification.Shader ? m_Specification.Shader->GetName() : "<none>" );
            return;
        }

        // A graphics pipeline is built against its target's render pass, so there is nothing to build
        // without one. This THREW `std::runtime_error` from CreateGraphicsPipeline, five function calls
        // further down and after a pipeline layout had already been created and leaked; nothing in the
        // engine catches it, so the refusal was std::terminate. Refusing here leaves the handle null,
        // which is the one signal every caller of this class already reads.
        if ( !m_Specification.Framebuffer )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: no target framebuffer. The draws using it are "
                       "skipped.",
                       m_Specification.DebugName );
            return;
        }

        // WHAT THE STAGE LIST CONTAINS, NOT MERELY THAT IT IS NOT EMPTY — the same question
        // VulkanPipelineCompute asks with GetComputeStage, from the other side. A compute program
        // reached by name compiles, so IsCompiled() says yes, and its stage bit is one
        // vkCreateGraphicsPipelines does not accept.
        VulkanShader* vulkanShader =
             std::static_pointer_cast<Graphic::API::Vulkan::VulkanShader>( m_Specification.Shader ).get();
        if ( !vulkanShader->GetVertexStage() )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: shader '{}' has no VERTEX stage — a graphics "
                       "pipeline cannot be built from it. The draws using it are skipped.",
                       m_Specification.DebugName, m_Specification.Shader->GetName() );
            return;
        }

        CreatePipelineLayout();
        CreateVertexInputState();
        CreateInputAssemblyState();
        CreateDynamicState();
        CreateViewportState();
        CreateRasterizationState();
        CreateMultisampleState();
        CreateDepthStencilState();
        CreateColorBlendState();

        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        CreateGraphicsPipeline( device, vulkanShader );

        LOG_INFO( "Created {} VulkanPipeline", m_Specification.DebugName );
    }

    void VulkanPipeline::CreatePipelineLayout()
    {
        VulkanShader* vulkanShader =
             std::static_pointer_cast<Graphic::API::Vulkan::VulkanShader>( m_Specification.Shader ).get();

        // Captured and KEPT for the life of this pipeline layout: a shader recompile publishes new
        // layouts and drops its own references, and a pipeline layout built from destroyed ones is the
        // "VkPipelineLayout references a VkDescriptorSetLayout that has been destroyed" the validation
        // layer reports on every bind. See VulkanDescriptorSetLayout.hpp.
        m_Layouts = vulkanShader->GetAllDescriptorSetLayouts();

        const std::vector<VkDescriptorSetLayout> descriptorSetLayouts = RawHandles( m_Layouts );
        const auto&                              pushConstant         = SetUpPushConstantRange();

        VkPipelineLayoutCreateInfo layoutInfo = {
             .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             .setLayoutCount         = static_cast<uint32_t>( descriptorSetLayouts.size() ),
             .pSetLayouts            = descriptorSetLayouts.data(),
             .pushConstantRangeCount = pushConstant.first,
             .pPushConstantRanges    = pushConstant.first > 0 ? &pushConstant.second : nullptr };

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();        VK_CHECK_RESULT( vkCreatePipelineLayout( device, &layoutInfo, nullptr, &m_PipelineLayout ) );
    }

    void VulkanPipeline::CreateVertexInputState()
    {
        if ( m_Specification.PullingConfig )
        {
            m_VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
                 .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                 .vertexBindingDescriptionCount   = 0,
                 .pVertexBindingDescriptions      = nullptr,
                 .vertexAttributeDescriptionCount = 0,
                 .pVertexAttributeDescriptions    = nullptr };
            return;
        }

        if ( !m_Specification.Layout || m_Specification.Layout->GetElementCount() == 0 )
        {
            m_VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
                 .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                 .vertexBindingDescriptionCount   = 0,
                 .pVertexBindingDescriptions      = nullptr,
                 .vertexAttributeDescriptionCount = 0,
                 .pVertexAttributeDescriptions    = nullptr };
            return;
        }

        m_VertexInputBinding = VkVertexInputBindingDescription{ .binding   = 0,
                                                                .stride    = m_Specification.Layout->GetStride(),
                                                                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        m_VertexAttributes.clear();
        const auto& layout = m_Specification.Layout.value();
        for ( uint32_t location = 0; const auto& element : layout )
        {
            m_VertexAttributes.push_back(
                 VkVertexInputAttributeDescription{ .location = location,
                                                    .binding  = 0,
                                                    .format   = ShaderDataTypeToVulkanFormat( element.Type ),
                                                    .offset   = element.Offset } );
            location++;
        }

        m_VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
             .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
             .vertexBindingDescriptionCount   = 1,
             .pVertexBindingDescriptions      = &m_VertexInputBinding,
             .vertexAttributeDescriptionCount = static_cast<uint32_t>( m_VertexAttributes.size() ),
             .pVertexAttributeDescriptions    = m_VertexAttributes.data() };
    }

    void VulkanPipeline::CreateInputAssemblyState()
    {
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        switch ( m_Specification.Topology )
        {
            case PrimitiveTopology::Points:        topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
            case PrimitiveTopology::Lines:         topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case PrimitiveTopology::LineStrip:    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
            case PrimitiveTopology::Triangles:     topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
            case PrimitiveTopology::TriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case PrimitiveTopology::TriangleFan:   topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
            case PrimitiveTopology::Patches:       topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
        }

        m_InputAssembly = VkPipelineInputAssemblyStateCreateInfo{
             .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
             .topology               = topology,
             .primitiveRestartEnable = VK_FALSE };
    }

    void VulkanPipeline::CreateDynamicState()
    {
        m_DynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };

        m_DynamicStateInfo = VkPipelineDynamicStateCreateInfo{
             .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
             .dynamicStateCount = static_cast<uint32_t>( m_DynamicStates.size() ),
             .pDynamicStates    = m_DynamicStates.data() };
    }

    void VulkanPipeline::CreateViewportState()
    {
        m_ViewportState =
             VkPipelineViewportStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                .viewportCount = 1,
                                                .pViewports    = nullptr,
                                                .scissorCount  = 1,
                                                .pScissors     = nullptr };
    }

    void VulkanPipeline::CreateRasterizationState()
    {
        m_Rasterizer = VkPipelineRasterizationStateCreateInfo{
             .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
             .depthClampEnable        = VK_FALSE,
             .rasterizerDiscardEnable = VK_FALSE,
             .polygonMode             = ConvertVkPolygonMode( m_Specification.PolygonMode ),
             .cullMode                = ConvertCullMode( m_Specification.CullMode ),
             .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
             .depthBiasEnable         = VK_FALSE,
             .lineWidth               = m_Specification.LineWidth };
    }

    void VulkanPipeline::CreateMultisampleState()
    {
        // The pipeline must rasterize at its target framebuffer's sample count (MSAA) — a
        // mismatch is a validation error and a black frame.
        const uint32_t samples =
             m_Specification.Framebuffer ? m_Specification.Framebuffer->GetSpecification().Samples : 1;
        m_Multisampling = VkPipelineMultisampleStateCreateInfo{
             .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
             .rasterizationSamples = static_cast<VkSampleCountFlagBits>( samples > 1 ? samples : 1 ),
             .sampleShadingEnable  = VK_FALSE };
    }

    void VulkanPipeline::CreateDepthStencilState()
    {
        VkStencilOpState frontStencil = ConvertStencilOpState( m_Specification.StencilFront );
        VkStencilOpState backStencil  = ConvertStencilOpState( m_Specification.StencilBack );

        m_DepthStencil = VkPipelineDepthStencilStateCreateInfo{
             .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
             .depthTestEnable       = m_Specification.DepthTestEnabled ? VK_TRUE : VK_FALSE,
             .depthWriteEnable      = m_Specification.DepthWriteEnabled ? VK_TRUE : VK_FALSE,
             .depthCompareOp        = ConvertCompareOp( m_Specification.DepthCompareOp ),
             .depthBoundsTestEnable = VK_FALSE,
             .stencilTestEnable     = m_Specification.StencilTestEnabled ? VK_TRUE : VK_FALSE,
             .front                 = frontStencil,
             .back                  = backStencil,
             .minDepthBounds        = 0.0f,
             .maxDepthBounds        = 1.0f };
    }

    void VulkanPipeline::CreateColorBlendState()
    {
        m_ColorBlendAttachments.clear();
        uint32_t colorAttachmentCount =
             m_Specification.Framebuffer ? m_Specification.Framebuffer->GetColorAttachmentCount() : 1;

        const VkBool32       blend  = m_Specification.BlendEnable ? VK_TRUE : VK_FALSE;
        const VkBlendFactor  srcCol = ConvertBlendFactor( m_Specification.SrcColorBlendFactor );
        const VkBlendFactor  dstCol = ConvertBlendFactor( m_Specification.DstColorBlendFactor );

        m_ColorBlendAttachments.resize( colorAttachmentCount );
        for ( auto& attachment : m_ColorBlendAttachments )
        {
            attachment = { .blendEnable         = blend,
                           .srcColorBlendFactor = srcCol,
                           .dstColorBlendFactor = dstCol,
                           .colorBlendOp        = VK_BLEND_OP_ADD,
                           .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                           .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                           .alphaBlendOp        = VK_BLEND_OP_ADD,
                           .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        }

        m_ColorBlending = VkPipelineColorBlendStateCreateInfo{
             .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
             .logicOpEnable   = VK_FALSE,
             .logicOp         = VK_LOGIC_OP_COPY,
             .attachmentCount = static_cast<uint32_t>( m_ColorBlendAttachments.size() ),
             .pAttachments    = m_ColorBlendAttachments.data(),
             .blendConstants  = { 0.0f, 0.0f, 0.0f, 0.0f } };
    }

    void VulkanPipeline::CreateGraphicsPipeline( VkDevice device, VulkanShader* vulkanShader )
    {
        // The `throw std::runtime_error( "Framebuffer is required for pipeline creation" )` that stood
        // here is now a REFUSAL at the top of Invalidate, before a pipeline layout is created and
        // leaked. It was never catchable: nothing in the engine catches, so it was std::terminate
        // wearing an error message.
        const auto vkFb = std::static_pointer_cast<API::Vulkan::VulkanFramebuffer>( m_Specification.Framebuffer );
        VkRenderPass renderPass =
             m_Specification.UseLoadRenderPass ? vkFb->GetVKRenderPassLoad() : vkFb->GetVKRenderPass();

        // Tessellation: patch-list topology needs a tessellation state (control points per patch).
        VkPipelineTessellationStateCreateInfo tessellationState = {
             .sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
             .patchControlPoints = m_Specification.PatchControlPoints };
        const bool usesTessellation = m_Specification.PatchControlPoints > 0;

        VkGraphicsPipelineCreateInfo pipelineInfo = {
             .sType      = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
             .stageCount = static_cast<uint32_t>( vulkanShader->GetPipelineShaderStageCreateInfos().size() ),
             .pStages    = vulkanShader->GetPipelineShaderStageCreateInfos().data(),
             .pVertexInputState   = &m_VertexInputInfo,
             .pInputAssemblyState = &m_InputAssembly,
             .pTessellationState  = usesTessellation ? &tessellationState : nullptr,
             .pViewportState      = &m_ViewportState,
             .pRasterizationState = &m_Rasterizer,
             .pMultisampleState   = &m_Multisampling,
             .pDepthStencilState  = &m_DepthStencil,
             .pColorBlendState    = &m_ColorBlending,
             .pDynamicState       = &m_DynamicStateInfo,
             .layout              = m_PipelineLayout,
             .renderPass          = renderPass,
             .subpass             = 0,
             .basePipelineHandle  = VK_NULL_HANDLE,
             .basePipelineIndex   = -1 };

        // Use the device-wide, disk-persisted pipeline cache so the driver reuses previously-built
        // pipeline binaries across runs instead of compiling this graphics pipeline from scratch.
        const VkPipelineCache pipelineCache =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetPipelineCache();
        {
            const Core::ScopedShaderPhase timer( Core::ShaderPhase::PipelineCreate );
            VK_CHECK_RESULT(
                 vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineInfo, nullptr, &m_Pipeline ) );
        }

        // Debug name so RenderDoc/validation identify the pipeline by its spec name.
        if ( !m_Specification.DebugName.empty() )
        {
            VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE, m_Specification.DebugName,
                                              m_Pipeline );
        }
    }

    std::pair<uint32_t, VkPushConstantRange> VulkanPipeline::SetUpPushConstantRange() const
    {
        VulkanShader* vulkanShader =
             std::static_pointer_cast<Graphic::API::Vulkan::VulkanShader>( m_Specification.Shader ).get();

        const auto& pushConstant = vulkanShader->GetShaderPushConstant();
        if ( !pushConstant )
        {
            return { 0, {} };
        }

        const auto&          pcValue = pushConstant.value();
        VkPushConstantRange pushConstantCI;
        pushConstantCI.offset     = pcValue.Offset;
        pushConstantCI.size       = pcValue.Size;
        pushConstantCI.stageFlags = (VkShaderStageFlags)pcValue.ShaderStage;

        return { 1, pushConstantCI };
    }

    bool VulkanPipeline::HasDepth()
    {
        const auto& attachments = m_Specification.Framebuffer->GetSpecification().Attachments.Attachments;
        return std::any_of( attachments.begin(), attachments.end(),
                            []( const auto& att ) { return Graphic::Utils::IsDepthFormat( att.Format ); } );
    }

    VkStencilOpState VulkanPipeline::ConvertStencilOpState( const StencilOpState& state )
    {
        return VkStencilOpState{ .failOp      = ConvertStencilOp( state.FailOp ),
                                 .passOp      = ConvertStencilOp( state.PassOp ),
                                 .depthFailOp = ConvertStencilOp( state.DepthFailOp ),
                                 .compareOp   = ConvertCompareOp( state.CompareOp ),
                                 .compareMask = state.CompareMask,
                                 .writeMask   = state.WriteMask,
                                 .reference   = state.Reference };
    }

} // namespace Desert::Graphic::API::Vulkan

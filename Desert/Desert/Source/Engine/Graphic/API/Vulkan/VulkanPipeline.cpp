#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/Image.hpp>

#include <algorithm>
#include <format>
#include <optional>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        thread_local std::string_view t_PipelineBeingCreated;

        // Names the pipeline for the debug callback for the duration of one vkCreateGraphicsPipelines.
        class PipelineCreateScope
        {
        public:
            explicit PipelineCreateScope( std::string_view name )
            {
                t_PipelineBeingCreated = name;
            }
            ~PipelineCreateScope()
            {
                t_PipelineBeingCreated = {};
            }
            PipelineCreateScope( const PipelineCreateScope& )            = delete;
            PipelineCreateScope& operator=( const PipelineCreateScope& ) = delete;
        };
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
    }

    VulkanPipeline::VulkanPipeline( const GraphicsPipelineSpecification& specification )
         : m_Specification( specification ), m_Owned( NeverBuilt() )
    {
        m_Current.store( m_Owned.get(), std::memory_order_release );
    }

    VulkanPipeline::~VulkanPipeline()
    {
        // At once, not deferred: whoever drops the last owner of a pipeline has no frame recording with it
        // (every recorder holds its pipeline strongly), the same rule as a shader's last record.
        Unbuild( false );
    }

    const std::shared_ptr<const VulkanPipeline::BuiltPipeline>& VulkanPipeline::NeverBuilt() noexcept
    {
        // One process-wide empty record: no handle, so GetVkPipelineFor never reaches its variants.
        static const std::shared_ptr<const BuiltPipeline> s_Empty = std::make_shared<const BuiltPipeline>();
        return s_Empty;
    }

    VulkanBuiltPipeline::~VulkanBuiltPipeline()
    {
        if ( Pipeline == VK_NULL_HANDLE && PipelineLayout == VK_NULL_HANDLE &&
             CompatibleRenderPass == VK_NULL_HANDLE && PassVariants.empty() )
            return;

        VkDevice device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )
                              ->GetVulkanLogicalDevice();
        for ( const auto& [pass, variant] : PassVariants )
            if ( variant != VK_NULL_HANDLE )
                vkDestroyPipeline( device, variant, nullptr );
        if ( Pipeline != VK_NULL_HANDLE )
            vkDestroyPipeline( device, Pipeline, nullptr );
        if ( PipelineLayout != VK_NULL_HANDLE )
            vkDestroyPipelineLayout( device, PipelineLayout, nullptr );
        if ( CompatibleRenderPass != VK_NULL_HANDLE )
            vkDestroyRenderPass( device, CompatibleRenderPass, nullptr );
        // Layouts and Program are released after the pipeline layout (member destruction follows this body):
        // it was built from them.
    }

    void VulkanPipeline::Publish( std::shared_ptr<const BuiltPipeline> next, const bool deferred ) noexcept
    {
        std::shared_ptr<const BuiltPipeline> replaced = std::move( m_Owned );
        m_Owned                                       = std::move( next );
        if ( !m_Owned )
            m_Owned = NeverBuilt();
        m_Current.store( m_Owned.get(), std::memory_order_release );

        if ( !replaced || replaced == NeverBuilt() )
            return; // nothing was built, or the shared empty record: nothing to retire

        // THE OLD RECORD OUTLIVES EVERY FRAME THAT MAY STILL READ IT: a render worker of frame N-1 may be
        // binding its VkPipeline or creating a pass variant from its create-info while this rebuild runs in
        // frame N. It goes through the frame ring's deletion queue, and only at once with no renderer.
        if ( !deferred )
            return; // `replaced` dies here
        const auto ctx       = EngineContext::GetInstance().GetRendererContext();
        auto*      allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
        if ( allocator != nullptr )
            allocator->RT_DestroyLater( [held = std::move( replaced )]() mutable { held.reset(); } );
    }

    void VulkanPipeline::Unbuild( const bool deferred ) noexcept
    {
        // A compile on a worker writes the record being built and publishes it: it must be over before either
        // is touched.
        if ( m_Compile.valid() )
            m_Compile.wait();
        m_Compile = {};
        m_State.store( BuildState::Unbuilt, std::memory_order_release );
        ::Desert::Graphic::PipelineReadinessCounter().fetch_add( 1u, std::memory_order_acq_rel );
        m_Building.reset();
        Publish( NeverBuilt(), deferred );
    }

    void VulkanPipeline::Release()
    {
        Unbuild( true );
    }

    void VulkanPipeline::Invalidate()
    {
        Build( CompileOn::CallingThread );
    }

    void VulkanPipeline::InvalidateAsync( const PipelineRole role )
    {
        m_Role = role;
        Build( CompileOn::Worker );
    }

    void VulkanPipeline::Build( const CompileOn where )
    {
        Release();
        RecordShaderCodeGeneration();

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
        //
        // THE PROGRAM IS READ ONCE, co-owned, and every part of the build below comes from that one record: a
        // shader reload landing mid-build cannot pair one compile's stages with the next one's layouts, and the
        // stage infos a pass variant is created from later stay alive with this pipeline's record.
        std::shared_ptr<const VulkanShader::CompiledProgram> program;
        if ( m_Specification.Shader )
            program = std::static_pointer_cast<Graphic::API::Vulkan::VulkanShader>( m_Specification.Shader )
                           ->ShareProgram();
        if ( !program || program->StageInfos.empty() )
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
        if ( !m_Specification.Framebuffer == !m_Specification.TargetLayout )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: it needs exactly one target, a Framebuffer or a "
                       "TargetLayout (has {}). The draws using it are skipped.",
                       m_Specification.DebugName, m_Specification.Framebuffer ? "both" : "neither" );
            return;
        }

        // WHAT THE STAGE LIST CONTAINS, NOT MERELY THAT IT IS NOT EMPTY — the same question
        // VulkanPipelineCompute asks with GetComputeStage, from the other side. A compute program
        // reached by name compiles, so IsCompiled() says yes, and its stage bit is one
        // vkCreateGraphicsPipelines does not accept.
        const bool hasVertexStage =
             std::ranges::any_of( program->StageInfos, []( const VkPipelineShaderStageCreateInfo& stage )
                                  { return stage.stage == VK_SHADER_STAGE_VERTEX_BIT; } );
        if ( !hasVertexStage )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: shader '{}' has no VERTEX stage — a graphics "
                       "pipeline cannot be built from it. The draws using it are skipped.",
                       m_Specification.DebugName, m_Specification.Shader->GetName() );
            return;
        }

        // A NEW RECORD: nothing below writes into the one readers hold (Publish swaps it in when compiled).
        m_Building          = std::make_shared<BuiltPipeline>();
        m_Building->Program = std::move( program );

        // The vertex input is decided before anything is created: a stage reading a location the layout does
        // not feed (or an attribute with no Vulkan format) is a refusal, like the missing VERTEX stage above —
        // the pipeline stays unbuilt and nothing draws, rather than being built over undefined input.
        if ( !CreateVertexInputState() )
        {
            m_State.store( BuildState::Failed, std::memory_order_release );
            ::Desert::Graphic::PipelineReadinessCounter().fetch_add( 1u, std::memory_order_acq_rel );
            return;
        }
        CreatePipelineLayout();
        CreateInputAssemblyState();
        CreateDynamicState();
        CreateViewportState();
        CreateRasterizationState();
        CreateMultisampleState();
        CreateDepthStencilState();
        CreateColorBlendState();

        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        CreateGraphicsPipeline( device, where );
    }

    void VulkanPipeline::CreatePipelineLayout()
    {
        const VulkanShader::CompiledProgram& program = *m_Building->Program;

        // Captured and KEPT for the life of this pipeline layout: a shader recompile publishes new
        // layouts and drops its own references, and a pipeline layout built from destroyed ones is the
        // "VkPipelineLayout references a VkDescriptorSetLayout that has been destroyed" the validation
        // layer reports on every bind. See VulkanDescriptorSetLayout.hpp.
        m_Building->Layouts = program.SetLayouts;

        const std::vector<VkDescriptorSetLayout> descriptorSetLayouts = RawHandles( m_Building->Layouts );
        const auto&                              pushConstant         = SetUpPushConstantRange();

        VkPipelineLayoutCreateInfo layoutInfo = {
             .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             .setLayoutCount         = static_cast<uint32_t>( descriptorSetLayouts.size() ),
             .pSetLayouts            = descriptorSetLayouts.data(),
             .pushConstantRangeCount = pushConstant.first,
             .pPushConstantRanges    = pushConstant.first > 0 ? &pushConstant.second : nullptr };

        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();
        VK_CHECK_RESULT( vkCreatePipelineLayout( device, &layoutInfo, nullptr, &m_Building->PipelineLayout ) );
        m_Building->PushConstantRange =
             pushConstant.first > 0 ? std::optional( pushConstant.second ) : std::nullopt;
        // Named like the pipeline, so a validation message about a layout (push ranges, set layouts) names the
        // pipeline it belongs to rather than a bare handle.
        if ( !m_Specification.DebugName.empty() )
            VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, m_Specification.DebugName,
                                              m_Building->PipelineLayout );
    }

    bool VulkanPipeline::CreateVertexInputState()
    {
        m_Building->VertexInput = {};
        if ( m_Specification.PullingConfig )
        {
            m_Building->VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
                 .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                 .vertexBindingDescriptionCount   = 0,
                 .pVertexBindingDescriptions      = nullptr,
                 .vertexAttributeDescriptionCount = 0,
                 .pVertexAttributeDescriptions    = nullptr };
            return true;
        }

        if ( !m_Specification.Layout || m_Specification.Layout->GetElementCount() == 0 )
        {
            m_Building->VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
                 .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                 .vertexBindingDescriptionCount   = 0,
                 .pVertexBindingDescriptions      = nullptr,
                 .vertexAttributeDescriptionCount = 0,
                 .pVertexAttributeDescriptions    = nullptr };
            return true;
        }

        // Layout ∩ what the vertex stage reads (ShaderReflection::BuildVertexInput): no attribute for a
        // location the shader does not declare, no binding 1 when none of its streams is read.
        const VulkanShader::CompiledProgram& program = *m_Building->Program;
        m_Building->VertexInput =
             ShaderReflection::BuildVertexInput( m_Specification.Layout.value(), program.Reflection.VertexInputs );
        if ( const auto refusal = ShaderReflection::VertexInputRefusal( m_Building->VertexInput ) )
        {
            LOG_ERROR( "[Pipeline] '{}' not created: shader '{}' does not match its vertex layout ({}). The "
                       "draws using it are skipped.",
                       m_Specification.DebugName, m_Specification.Shader->GetName(), *refusal );
            return false;
        }

        m_Building->VertexInputInfo = VkPipelineVertexInputStateCreateInfo{
             .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
             .pNext                           = nullptr,
             .flags                           = 0,
             .vertexBindingDescriptionCount   = static_cast<uint32_t>( m_Building->VertexInput.Bindings.size() ),
             .pVertexBindingDescriptions      = m_Building->VertexInput.Bindings.data(),
             .vertexAttributeDescriptionCount = static_cast<uint32_t>( m_Building->VertexInput.Attributes.size() ),
             .pVertexAttributeDescriptions    = m_Building->VertexInput.Attributes.data() };
        return true;
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

        m_Building->InputAssembly = VkPipelineInputAssemblyStateCreateInfo{
             .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
             .topology               = topology,
             .primitiveRestartEnable = VK_FALSE };
    }

    void VulkanPipeline::CreateDynamicState()
    {
        m_Building->DynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_LINE_WIDTH };

        m_Building->DynamicStateInfo = VkPipelineDynamicStateCreateInfo{
             .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
             .dynamicStateCount = static_cast<uint32_t>( m_Building->DynamicStates.size() ),
             .pDynamicStates    = m_Building->DynamicStates.data() };
    }

    void VulkanPipeline::CreateViewportState()
    {
        m_Building->ViewportState =
             VkPipelineViewportStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                .viewportCount = 1,
                                                .pViewports    = nullptr,
                                                .scissorCount  = 1,
                                                .pScissors     = nullptr };
    }

    void VulkanPipeline::CreateRasterizationState()
    {
        m_Building->Rasterizer = VkPipelineRasterizationStateCreateInfo{
             .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
             .depthClampEnable        = m_Specification.DepthClampEnable ? VK_TRUE : VK_FALSE,
             .rasterizerDiscardEnable = VK_FALSE,
             .polygonMode             = ConvertVkPolygonMode( m_Specification.PolygonMode ),
             .cullMode                = ConvertCullMode( m_Specification.CullMode ),
             .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
             .depthBiasEnable = m_Specification.DepthBiasConstant != 0.0F || m_Specification.DepthBiasSlope != 0.0F
                                     ? VK_TRUE
                                     : VK_FALSE,
             .depthBiasConstantFactor = m_Specification.DepthBiasConstant,
             .depthBiasClamp          = 0.0F,
             .depthBiasSlopeFactor    = m_Specification.DepthBiasSlope,
             .lineWidth               = m_Specification.LineWidth };
    }

    void VulkanPipeline::CreateMultisampleState()
    {
        // The pipeline must rasterize at its target framebuffer's sample count (MSAA) — a
        // mismatch is a validation error and a black frame.
        uint32_t samples = 1;
        if ( m_Specification.Framebuffer )
            samples = m_Specification.Framebuffer->GetSpecification().Samples;
        else if ( m_Specification.TargetLayout.has_value() )
            samples = m_Specification.TargetLayout->Samples;
        m_Building->BuiltSamples        = samples > 1 ? samples : 1;
        m_Building->Multisampling       = {};
        m_Building->Multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        m_Building->Multisampling.rasterizationSamples =
             static_cast<VkSampleCountFlagBits>( m_Building->BuiltSamples );
        m_Building->Multisampling.sampleShadingEnable = VK_FALSE;
    }

    RdgRenderPassKey CompatibleRenderPassKeyOf( const FramebufferSpecification& spec, const uint32_t samples )
    {
        std::vector<VkFormat>                     colourFormats;
        std::optional<Core::Formats::ImageFormat> depth;
        for ( const auto& attachment : spec.Attachments.Attachments )
        {
            if ( attachment.Unused )
                colourFormats.push_back( VK_FORMAT_UNDEFINED ); // an unused colour slot: VK_ATTACHMENT_UNUSED
            else if ( Graphic::Utils::IsDepthFormat( attachment.Format ) )
                depth = attachment.Format;
            else
                colourFormats.push_back( API::Vulkan::GetImageVulkanFormat( attachment.Format ) );
        }
        const VkFormat depthFormat = depth ? API::Vulkan::GetImageVulkanFormat( *depth ) : VK_FORMAT_UNDEFINED;
        const bool     hasStencil =
             depth && ( API::Vulkan::GetImageVulkanAspect( *depth ) & VK_IMAGE_ASPECT_STENCIL_BIT ) != 0;
        return RdgCompatibleRenderPassKey( colourFormats, depthFormat, hasStencil, std::max( 1u, samples ) );
    }

    std::string_view PipelineBeingCreatedOnThisThread()
    {
        return t_PipelineBeingCreated;
    }

    VkPipeline VulkanPipeline::GetVkPipelineFor( const RdgRenderPassKey& openPass )
    {
        return GetVkPipelineFor( Current(), openPass );
    }

    VkPipeline VulkanPipeline::GetVkPipelineFor( const BuiltPipeline& record, const RdgRenderPassKey& openPass )
    {
        // ONE RECORD for the whole lookup: the base, its sample count, the create-info a variant is made
        // from and the variant map all belong to the build the caller read.
        if ( GetBuildState() != BuildState::Built )
            return VK_NULL_HANDLE;
        VkPipeline           base    = record.Pipeline;
        const uint32_t samples = std::max( 1u, openPass.Samples );
        if ( base == VK_NULL_HANDLE || samples == record.BuiltSamples )
            return base;
        const std::lock_guard<std::mutex> variants( record.PassVariantsLock );
        if ( const auto found = record.PassVariants.find( openPass );
             found != record.PassVariants.end() )
            return found->second;

        VkDevice device =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        // A render pass of the open pass's compatibility class. A pipeline does not keep its render pass, so
        // this one is destroyed right after the create.
        const Common::ResultStr<VkRenderPass> pass = API::Vulkan::CreateRdgRenderPass( device, openPass );
        if ( !pass )
        {
            LOG_ERROR( "[Pipeline] '{}': no {}x variant, its render pass was not created: {}",
                       m_Specification.DebugName, samples, pass.GetError() );
            record.PassVariants.emplace( openPass, VK_NULL_HANDLE );
            return VK_NULL_HANDLE;
        }

        VkPipelineMultisampleStateCreateInfo multisampling = record.Multisampling;
        multisampling.rasterizationSamples                 = static_cast<VkSampleCountFlagBits>( samples );
        VkGraphicsPipelineCreateInfo info                  = record.PipelineInfo;
        info.pMultisampleState                             = &multisampling;
        info.renderPass                                    = pass.GetValue();

        VkPipeline      variant = VK_NULL_HANDLE;
        VkPipelineCache cache =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetPipelineCache();
        const std::string variantName = std::format( "{} ({}x)", m_Specification.DebugName, samples );
        VkResult          result{};
        {
            const PipelineCreateScope naming( variantName );
            result = vkCreateGraphicsPipelines( device, cache, 1, &info, nullptr, &variant );
        }
        vkDestroyRenderPass( device, pass.GetValue(), nullptr );
        if ( result != VK_SUCCESS )
        {
            LOG_ERROR( "[Pipeline] '{}': the {}x variant was not created: vkCreateGraphicsPipelines = {}",
                       m_Specification.DebugName, samples, VkResultToString( result ) );
            variant = VK_NULL_HANDLE;
        }
        else if ( !m_Specification.DebugName.empty() )
        {
            VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE, variantName, variant );
        }
        record.PassVariants.emplace( openPass, variant );
        return variant;
    }

    void VulkanPipeline::CreateDepthStencilState()
    {
        VkStencilOpState frontStencil = ConvertStencilOpState( m_Specification.StencilFront );
        VkStencilOpState backStencil  = ConvertStencilOpState( m_Specification.StencilBack );

        m_Building->DepthStencil = VkPipelineDepthStencilStateCreateInfo{
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
        m_Building->ColorBlendAttachments.clear();
        // Per attachment, by its format: an integer target never blends (ColourAttachmentBlendEnables).
        const std::vector<bool> blends = ColourAttachmentBlendEnables( ColourAttachmentFormats( m_Specification ),
                                                                       m_Specification.BlendEnable );

        // Per slot: blending from the attachment's FORMAT (above), the write mask from what the fragment stage
        // writes — a slot the stage does not write is masked (write mask 0), so a target carrying the view's
        // velocity next to scene colour keeps the velocity the depth-writing passes left there.
        const VulkanShader::CompiledProgram& program = *m_Building->Program;
        m_Building->ColorBlendAttachments = ShaderReflection::BuildColorBlendAttachments(
             program.Reflection.FragmentOutputLocations, blends,
             ConvertBlendFactor( m_Specification.SrcColorBlendFactor ),
             ConvertBlendFactor( m_Specification.DstColorBlendFactor ),
             ConvertBlendFactor( m_Specification.SrcAlphaBlendFactor ),
             ConvertBlendFactor( m_Specification.DstAlphaBlendFactor ) );

        m_Building->ColorBlending = VkPipelineColorBlendStateCreateInfo{
             .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
             .logicOpEnable   = VK_FALSE,
             .logicOp         = VK_LOGIC_OP_COPY,
             .attachmentCount = static_cast<uint32_t>( m_Building->ColorBlendAttachments.size() ),
             .pAttachments    = m_Building->ColorBlendAttachments.data(),
             .blendConstants  = { 0.0f, 0.0f, 0.0f, 0.0f } };
    }

    void VulkanPipeline::CreateGraphicsPipeline( VkDevice device, const CompileOn where )
    {
        // The `throw std::runtime_error( "Framebuffer is required for pipeline creation" )` that stood
        // here is now a REFUSAL at the top of Invalidate, before a pipeline layout is created and
        // leaked. It was never catchable: nothing in the engine catches, so it was std::terminate
        // wearing an error message.
        VkRenderPass renderPass = VK_NULL_HANDLE;
        if ( m_Specification.Framebuffer )
        {
            const auto vkFb =
                 std::static_pointer_cast<API::Vulkan::VulkanFramebuffer>( m_Specification.Framebuffer );
            renderPass = m_Specification.UseLoadRenderPass ? vkFb->GetVKRenderPassLoad() : vkFb->GetVKRenderPass();
        }
        else if ( m_Specification.TargetLayout.has_value() )
        {
            // A render-graph pass: built against the canonical render pass of the TargetLayout.
            const RenderTargetLayout& layout = *m_Specification.TargetLayout;
            std::vector<VkFormat>     colourFormats;
            colourFormats.reserve( layout.ColorFormats.size() );
            // An unused colour slot is VK_FORMAT_UNDEFINED: CreateRdgRenderPass references it as
            // VK_ATTACHMENT_UNUSED.
            for ( const std::optional<Core::Formats::ImageFormat>& format : layout.ColorFormats )
                colourFormats.push_back( format ? API::Vulkan::GetImageVulkanFormat( *format )
                                                : VK_FORMAT_UNDEFINED );
            VkFormat depthFormat = VK_FORMAT_UNDEFINED;
            if ( layout.DepthFormat.has_value() )
                depthFormat = API::Vulkan::GetImageVulkanFormat( layout.DepthFormat.value() );
            const bool hasStencil =
                 layout.DepthFormat &&
                 ( API::Vulkan::GetImageVulkanAspect( *layout.DepthFormat ) & VK_IMAGE_ASPECT_STENCIL_BIT ) != 0;
            if ( m_Building->CompatibleRenderPass != VK_NULL_HANDLE )
                vkDestroyRenderPass( device, m_Building->CompatibleRenderPass, nullptr );
            m_Building->CompatibleRenderPass              = VK_NULL_HANDLE;
            const Common::ResultStr<VkRenderPass> created = API::Vulkan::CreateRdgRenderPass(
                 device, API::Vulkan::RdgCompatibleRenderPassKey( colourFormats, depthFormat, hasStencil,
                                                                  layout.Samples ) );
            if ( !created )
            {
                LOG_ERROR( "[Pipeline] '{}' not created: {}", m_Specification.DebugName, created.GetError() );
                return;
            }
            m_Building->CompatibleRenderPass = created.GetValue();
            renderPass                       = m_Building->CompatibleRenderPass;
        }

        // Tessellation: patch-list topology needs a tessellation state (control points per patch).
        m_Building->Tessellation    = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                        .pNext = nullptr,
                                        .flags = 0,
                                        .patchControlPoints = m_Specification.PatchControlPoints };
        const bool usesTessellation = m_Specification.PatchControlPoints > 0;

        // Members, not locals: a compile on a worker reads them after this function has returned.
        m_Building->PipelineInfo = {
             .sType      = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
             .pNext      = nullptr,
             .flags      = 0,
             .stageCount = static_cast<uint32_t>( m_Building->Program->StageInfos.size() ),
             .pStages    = m_Building->Program->StageInfos.data(),
             .pVertexInputState   = &m_Building->VertexInputInfo,
             .pInputAssemblyState = &m_Building->InputAssembly,
             .pTessellationState  = usesTessellation ? &m_Building->Tessellation : nullptr,
             .pViewportState      = &m_Building->ViewportState,
             .pRasterizationState = &m_Building->Rasterizer,
             .pMultisampleState   = &m_Building->Multisampling,
             .pDepthStencilState  = &m_Building->DepthStencil,
             .pColorBlendState    = &m_Building->ColorBlending,
             .pDynamicState       = &m_Building->DynamicStateInfo,
             .layout              = m_Building->PipelineLayout,
             .renderPass          = renderPass,
             .subpass             = 0,
             .basePipelineHandle  = VK_NULL_HANDLE,
             .basePipelineIndex   = -1 };

        // The device-wide, disk-persisted cache: the driver reuses binaries across runs. VkPipelineCache is
        // internally synchronized (the spec requires no external lock for vkCreate*Pipelines), so workers
        // share the one cache — no per-thread caches to merge.
        VkPipelineCache pipelineCache =
             SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetPipelineCache();
        // The record moves to the compile: from here on only the compile writes it, and publishes it.
        std::shared_ptr<BuiltPipeline> next = std::move( m_Building );
        if ( where == CompileOn::CallingThread )
        {
            Compile( device, pipelineCache, std::move( next ) );
            return;
        }
        m_State.store( BuildState::Compiling, std::memory_order_release );
        ::Desert::Graphic::PipelineReadinessCounter().fetch_add( 1u, std::memory_order_acq_rel );
        PipelineBuilds::Get().OnStarted( m_Role );
        m_Compile = Common::JobSystem::Get().Async(
             [this, device, pipelineCache, role = m_Role, next = std::move( next )]() mutable noexcept
             {
                 Compile( device, pipelineCache, std::move( next ) );
                 PipelineBuilds::Get().OnFinished( role );
             } );
    }

    void VulkanPipeline::Compile( VkDevice device, VkPipelineCache pipelineCache,
                                  std::shared_ptr<BuiltPipeline> next )
    {
        VkPipeline built = VK_NULL_HANDLE;
        VkResult   result{};
        {
            const Core::ScopedShaderPhase timer( Core::ShaderPhase::PipelineCreate );
            const PipelineCreateScope     naming( m_Specification.DebugName );
            result =
                 vkCreateGraphicsPipelines( device, pipelineCache, 1, &next->PipelineInfo, nullptr, &built );
        }
        if ( result != VK_SUCCESS )
        {
            // A refusal with the driver's code, not an assert: on a worker there is nobody to catch one,
            // and the draw path reports the unbuilt pipeline by name (VulkanRendererAPI::BindGraphicsPipeline).
            LOG_ERROR( "[Pipeline] '{}' not created: vkCreateGraphicsPipelines = {}", m_Specification.DebugName,
                       VkResultToString( result ) );
            // Published without a handle: the layout it did build is owned by the record, as before.
            Publish( std::move( next ), true );
            m_State.store( BuildState::Failed, std::memory_order_release );
            ::Desert::Graphic::PipelineReadinessCounter().fetch_add( 1u, std::memory_order_acq_rel );
            return;
        }

        // Debug name so RenderDoc/validation identify the pipeline by its spec name.
        if ( !m_Specification.DebugName.empty() )
            VKUtils::SetDebugUtilsObjectName( device, VK_OBJECT_TYPE_PIPELINE, m_Specification.DebugName, built );
        next->Pipeline = built;
        // Published BEFORE the state says Built (release on both): a reader that sees Built reads this record.
        Publish( std::move( next ), true );
        m_State.store( BuildState::Built, std::memory_order_release );
        ::Desert::Graphic::PipelineReadinessCounter().fetch_add( 1u, std::memory_order_acq_rel );
        LOG_INFO( "Created {} VulkanPipeline", m_Specification.DebugName );
    }

    std::pair<uint32_t, VkPushConstantRange> VulkanPipeline::SetUpPushConstantRange() const
    {
        const VulkanShader::CompiledProgram& program = *m_Building->Program;

        const auto& pushConstant = program.Reflection.PushConstantRanges;
        if ( !pushConstant )
        {
            return { 0, {} };
        }

        const auto&          pcValue = pushConstant.value();
        VkPushConstantRange pushConstantCI;
        pushConstantCI.offset     = pcValue.Offset;
        pushConstantCI.size       = ShaderResources::ShaderLayout::PushBlockSize( pushConstant );
        pushConstantCI.stageFlags = (VkShaderStageFlags)pcValue.ShaderStage;

        return { 1, pushConstantCI };
    }

    bool VulkanPipeline::HasDepth()
    {
        if ( !m_Specification.Framebuffer )
            return m_Specification.TargetLayout && m_Specification.TargetLayout->DepthFormat.has_value();
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

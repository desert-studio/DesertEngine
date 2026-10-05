#pragma once

#include <unordered_map>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/Framebuffer.hpp>

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <future>

namespace Desert::Graphic::API::Vulkan
{
    // The compatibility key of a render pass opened on a Framebuffer's attachments at `samples` -- what the
    // recording context hands GetVkPipelineFor for a framebuffer-opened pass.
    RdgRenderPassKey CompatibleRenderPassKeyOf( const FramebufferSpecification& spec, uint32_t samples );

    class VulkanPipeline final : public GraphicsPipeline
    {
    public:
        VulkanPipeline( const GraphicsPipelineSpecification& specification );
        ~VulkanPipeline() override;

        // Builds on the calling thread: the handle is final when this returns.
        virtual void Invalidate() override;
        // Everything but the driver compile on the calling thread; vkCreateGraphicsPipelines on the
        // JobSystem (PSO1). Until it lands, GetVkPipeline() is null and GetBuildState() is Compiling.
        // @p role decides whether the reveal waits for it (PipelineBuilds): engine passes yes, materials no.
        void         InvalidateAsync( PipelineRole role );
        virtual void Release() override;

        enum class BuildState : uint8_t
        {
            Unbuilt,   ///< never built, or refused before a compile started (reason logged)
            Compiling, ///< the driver compile is running on a worker
            Built,
            Failed, ///< the driver refused (reason logged)
        };
        BuildState GetBuildState() const
        {
            return m_State.load( std::memory_order_acquire );
        }

        // Unbuilt counts as Failed: a pipeline handed out unbuilt was refused, and its reason is logged.
        [[nodiscard]] PipelineReadiness GetReadiness() const override
        {
            switch ( GetBuildState() )
            {
                case BuildState::Compiling:
                    return PipelineReadiness::Compiling;
                case BuildState::Built:
                    return PipelineReadiness::Ready;
                case BuildState::Unbuilt:
                case BuildState::Failed:
                    return PipelineReadiness::Failed;
            }
            return PipelineReadiness::Failed;
        }

        [[nodiscard]] virtual PipelineType GetType() const override { return PipelineType::Graphics; }
        [[nodiscard]] virtual const std::shared_ptr<Shader>& GetShader() const override { return m_Specification.Shader; }

        [[nodiscard]] virtual const GraphicsPipelineSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        // Null until the compile has landed: a pipeline still in the driver is not drawn through.
        VkPipeline GetVkPipeline() const
        {
            return GetBuildState() == BuildState::Built ? m_Pipeline : VK_NULL_HANDLE;
        }

        // THE PIPELINE FOR THE RENDER PASS THE DRAW IS RECORDED IN, passed by the caller as that pass's
        // compatibility key (RdgCompatibilityKey). The pipeline is built at the sample count of its target when
        // it is created; a render pass at another count (the scene target after an anti-aliasing change) gets a
        // variant built against that pass, created here on first use and cached under its key until the
        // pipeline is rebuilt or released -- switching back costs nothing. Null while the base is not built or
        // when the variant could not be created (logged once per key).
        VkPipeline GetVkPipelineFor( const RdgRenderPassKey& openPass );

        // The descriptor set layouts m_PipelineLayout was built from, one per set.
        const std::vector<DescriptorSetLayoutRef>& GetLayouts() const
        {
            return m_Layouts;
        }
        VkPipelineLayout GetVkPipelineLayout() const
        {
            return m_PipelineLayout;
        }

    private:
        bool HasDepth();

    private:
        VkStencilOpState ConvertStencilOpState( const StencilOpState& state );

        void CreatePipelineLayout();
        void CreateVertexInputState();
        void CreateInputAssemblyState();
        void CreateDynamicState();
        void CreateViewportState();
        void CreateRasterizationState();
        void CreateMultisampleState();
        void CreateDepthStencilState();
        void CreateColorBlendState();

        enum class CompileOn : uint8_t
        {
            CallingThread,
            Worker,
        };
        void Build( CompileOn where );
        void CreateGraphicsPipeline( VkDevice device, VulkanShader* vulkanShader, CompileOn where );
        void Compile( VkDevice device, VkPipelineCache pipelineCache );

    private:
        std::pair<uint32_t, VkPushConstantRange> SetUpPushConstantRange() const;

    private:
        GraphicsPipelineSpecification m_Specification;

        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
        VkPipeline       m_Pipeline= VK_NULL_HANDLE;

        // The descriptor set layouts m_PipelineLayout was built from, held so they outlive it. A shader
        // recompile replaces the shader's references; this pipeline keeps its own until it is rebuilt.
        std::vector<DescriptorSetLayoutRef> m_Layouts;

        VkPipelineVertexInputStateCreateInfo   m_VertexInputInfo{};
        VkPipelineInputAssemblyStateCreateInfo m_InputAssembly{};
        VkPipelineDynamicStateCreateInfo       m_DynamicStateInfo{};
        VkPipelineViewportStateCreateInfo      m_ViewportState{};
        VkPipelineRasterizationStateCreateInfo m_Rasterizer{};
        VkPipelineMultisampleStateCreateInfo   m_Multisampling{};
        VkPipelineDepthStencilStateCreateInfo  m_DepthStencil{};
        VkPipelineColorBlendStateCreateInfo    m_ColorBlending{};
        VkVertexInputBindingDescription m_VertexInputBinding;

        std::vector<VkVertexInputAttributeDescription>   m_VertexAttributes;
        std::vector<VkDynamicState>                      m_DynamicStates;
        std::vector<VkPipelineColorBlendAttachmentState> m_ColorBlendAttachments;

        VkPipelineTessellationStateCreateInfo m_Tessellation{};
        VkGraphicsPipelineCreateInfo          m_PipelineInfo{};
        // TargetLayout: the canonical render pass (formats and samples, load/store DONT_CARE) the pipeline
        // is built against; compatible with every render pass the graph builds for those formats.
        VkRenderPass                          m_CompatibleRenderPass = VK_NULL_HANDLE;
        // The sample count m_Pipeline was built at, and the variants for every render pass at another count it
        // was bound in, by that pass's compatibility key.
        uint32_t                               m_BuiltSamples = 1;
        std::map<RdgRenderPassKey, VkPipeline> m_PassVariants;
        std::atomic<BuildState>               m_State{ BuildState::Unbuilt };
        PipelineRole                          m_Role = PipelineRole::Engine;
        std::future<void>                     m_Compile;
    };
} // namespace Desert::Graphic::API::Vulkan
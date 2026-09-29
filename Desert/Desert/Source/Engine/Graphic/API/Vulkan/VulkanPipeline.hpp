#pragma once

#include <array>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <future>

namespace Desert::Graphic::API::Vulkan
{
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
        // The variant a mesh draws through: its own streams at their stride, or the shared default at stride 0
        // (VertexBufferLayout::WithStreams). A layout without streams has one pipeline and answers it for both.
        VkPipeline GetVkPipeline( const bool meshHasStreams ) const
        {
            return meshHasStreams || !HasVertexStreams() ? GetVkPipeline()
                   : GetBuildState() == BuildState::Built ? m_PipelineNoStreams
                                                          : VK_NULL_HANDLE;
        }
        [[nodiscard]] bool HasVertexStreams() const
        {
            return !m_Specification.PullingConfig && m_Specification.Layout && m_Specification.Layout->HasStreams();
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
        VkPipeline       m_PipelineNoStreams = VK_NULL_HANDLE; // stride-0 twin; null without streams

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
        VkPipelineVertexInputStateCreateInfo           m_VertexInputInfoNoStreams{};
        std::array<VkVertexInputBindingDescription, 2> m_VertexInputBindings{};
        std::array<VkVertexInputBindingDescription, 2> m_VertexInputBindingsNoStreams{};

        std::vector<VkVertexInputAttributeDescription>   m_VertexAttributes;
        std::vector<VkDynamicState>                      m_DynamicStates;
        std::vector<VkPipelineColorBlendAttachmentState> m_ColorBlendAttachments;

        VkPipelineTessellationStateCreateInfo m_Tessellation{};
        VkGraphicsPipelineCreateInfo          m_PipelineInfo{};
        std::atomic<BuildState>               m_State{ BuildState::Unbuilt };
        PipelineRole                          m_Role = PipelineRole::Engine;
        std::future<void>                     m_Compile;
    };
} // namespace Desert::Graphic::API::Vulkan
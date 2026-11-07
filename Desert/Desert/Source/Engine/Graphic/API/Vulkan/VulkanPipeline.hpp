#pragma once

#include <array>
#include <unordered_map>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/Framebuffer.hpp>

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <optional>
#include <string_view>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>

namespace Desert::Graphic::API::Vulkan
{
    // The pipeline whose vkCreateGraphicsPipelines is running on THIS thread ("" outside a create). The
    // validation layer reports a create-time fault synchronously, before the pipeline has a debug name, and
    // pipelines compile on job workers whose "Created" lines interleave - the debug callback names the
    // pipeline from here instead.
    [[nodiscard]] std::string_view PipelineBeingCreatedOnThisThread();

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
            return GetBuildState() == BuildState::Built ? Current().Pipeline : VK_NULL_HANDLE;
        }
        // The pipeline's vertex input describes the optional streams binding (binding 1) — the layout has
        // streams AND the vertex stage reads one of them (ShaderReflection::BuildVertexInput): the draw
        // must bind a buffer there, and must not otherwise.
        [[nodiscard]] bool HasVertexStreams() const
        {
            return Current().VertexInput.HasBinding( 1 );
        }
        // The vertex stage reads the binned draw's instance id (a_VisibleRow, binding 2 at instance rate):
        // every draw with this pipeline binds the id stream there (VulkanRendererAPI::BindMeshBuffers).
        [[nodiscard]] bool ReadsVisibleRow() const
        {
            return Current().VertexInput.HasBinding( GpuDriven::kVisibleRowVertexBinding );
        }

        // THE PIPELINE FOR THE RENDER PASS THE DRAW IS RECORDED IN, passed by the caller as that pass's
        // compatibility key (RdgCompatibilityKey). The pipeline is built at the sample count of its target when
        // it is created; a render pass at another count (the scene target after an anti-aliasing change) gets a
        // variant built against that pass, created here on first use and cached under its key until the
        // pipeline is rebuilt or released -- switching back costs nothing. Null while the base is not built or
        // when the variant could not be created (logged once per key).
        VkPipeline GetVkPipelineFor( const RdgRenderPassKey& openPass );

        // The descriptor set layouts the published pipeline layout was built from, one per set.
        const std::vector<DescriptorSetLayoutRef>& GetLayouts() const
        {
            return Current().Layouts;
        }
        VkPipelineLayout GetVkPipelineLayout() const
        {
            return Current().PipelineLayout;
        }
        // The push range the pipeline layout was built with, and so the one a push into this pipeline must name
        // (stages, size). The shader's reflection is not that: the shader object outlives a recompile that
        // re-reflects it, while this pipeline keeps the layout it was built with until it is rebuilt, and a push
        // that named the shader's current stages named a stage this layout lacks
        // (VUID-vkCmdPushConstants-offset-01795). Empty when the layout declares no push range.
        const std::optional<VkPushConstantRange>& GetPushConstantRange() const
        {
            return Current().PushConstantRange;
        }

    private:
        bool HasDepth();

    private:
        VkStencilOpState ConvertStencilOpState( const StencilOpState& state );

        void CreatePipelineLayout();
        [[nodiscard]] bool CreateVertexInputState(); // false = refused (reason logged)
        void CreateInputAssemblyState();
        void CreateDynamicState();
        void CreateViewportState();
        void CreateRasterizationState();
        void CreateMultisampleState();
        void CreateDepthStencilState();
        void CreateColorBlendState();

        struct BuiltPipeline; // one build's product, published whole (below)

        enum class CompileOn : uint8_t
        {
            CallingThread,
            Worker,
        };
        void Build( CompileOn where );
        void CreateGraphicsPipeline( VkDevice device, CompileOn where );
        void Compile( VkDevice device, VkPipelineCache pipelineCache, std::shared_ptr<BuiltPipeline> next );

    private:
        std::pair<uint32_t, VkPushConstantRange> SetUpPushConstantRange() const;

    private:
        GraphicsPipelineSpecification m_Specification;

        // EVERYTHING ONE BUILD PRODUCED, AS ONE IMMUTABLE RECORD (UE: a PSO rebuilt after a shader change is a
        // new pipeline object; the old one is released by deferred deletion after the frames in flight). A
        // render worker of frame N-1 binds this pipeline - and creates its pass variants from the create-info
        // below - while the game thread of frame N rebuilds it after a shader reload; nothing it reads is ever
        // written again once published. A rebuild builds a NEW record and publishes it; the replaced one is
        // destroyed through the allocator's deletion queue (RT_DestroyLater), never in place.
        struct BuiltPipeline
        {
            // The shader record the stages came from, co-owned: PipelineInfo.pStages points into it, and a
            // pass variant is created from PipelineInfo frames after the shader may have been reloaded.
            std::shared_ptr<const VulkanShader::CompiledProgram> Program;

            VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
            VkPipeline       Pipeline       = VK_NULL_HANDLE;
            // Set with PipelineLayout, from the same SetUpPushConstantRange call (GetPushConstantRange).
            std::optional<VkPushConstantRange> PushConstantRange;
            // The descriptor set layouts PipelineLayout was built from, held so they outlive it.
            std::vector<DescriptorSetLayoutRef> Layouts;

            VkPipelineVertexInputStateCreateInfo   VertexInputInfo{};
            VkPipelineInputAssemblyStateCreateInfo InputAssembly{};
            VkPipelineDynamicStateCreateInfo       DynamicStateInfo{};
            VkPipelineViewportStateCreateInfo      ViewportState{};
            VkPipelineRasterizationStateCreateInfo Rasterizer{};
            VkPipelineMultisampleStateCreateInfo   Multisampling{};
            VkPipelineDepthStencilStateCreateInfo  DepthStencil{};
            VkPipelineColorBlendStateCreateInfo    ColorBlending{};
            // Layout ∩ vertex-stage inputs; VertexInputInfo points into it.
            ShaderReflection::VertexInputState VertexInput;

            std::vector<VkDynamicState>                      DynamicStates;
            std::vector<VkPipelineColorBlendAttachmentState> ColorBlendAttachments;

            VkPipelineTessellationStateCreateInfo Tessellation{};
            VkGraphicsPipelineCreateInfo          PipelineInfo{};
            // TargetLayout: the canonical render pass (formats and samples, load/store DONT_CARE) the pipeline
            // is built against; compatible with every render pass the graph builds for those formats.
            VkRenderPass CompatibleRenderPass = VK_NULL_HANDLE;
            // The sample count Pipeline was built at, and the variants for every render pass at another count
            // it was bound in, by that pass's compatibility key. The variants are the one part made after
            // publication (on first use, by a render worker), so they sit under their own lock and die with
            // the record they were made from.
            uint32_t                                       BuiltSamples = 1;
            mutable std::map<RdgRenderPassKey, VkPipeline> PassVariants;
            mutable std::mutex                             PassVariantsLock;

            BuiltPipeline()                                  = default;
            BuiltPipeline( const BuiltPipeline& )            = delete; // the create-info points into itself
            BuiltPipeline& operator=( const BuiltPipeline& ) = delete;
            ~BuiltPipeline(); // destroys the variants, the pipeline, its layout and the compatible render pass
        };

        [[nodiscard]] const BuiltPipeline& Current() const noexcept
        {
            return *m_Current.load( std::memory_order_acquire );
        }
        // Swap @p next in for readers; the record it replaces is destroyed after the frames in flight
        // (@p deferred) or at once (the destructor: the last owner has no frame recording with it).
        void Publish( std::shared_ptr<const BuiltPipeline> next, bool deferred ) noexcept;
        [[nodiscard]] static const std::shared_ptr<const BuiltPipeline>& NeverBuilt() noexcept;
        // Waits for a compile on a worker, marks the pipeline unbuilt and unpublishes its record.
        void Unbuild( bool deferred ) noexcept;

        // The record a build in progress writes (the calling thread, then the compile worker); published when
        // its compile lands. Never read by a reader.
        std::shared_ptr<BuiltPipeline> m_Building;
        // The published record: m_Owned is touched by the building thread only (Build/Release/the destructor,
        // after waiting for m_Compile, or the compile worker publishing); readers go through m_Current, never
        // null (the shared empty record while unbuilt).
        std::shared_ptr<const BuiltPipeline> m_Owned;
        std::atomic<const BuiltPipeline*>    m_Current{ nullptr };

        std::atomic<BuildState>               m_State{ BuildState::Unbuilt };
        PipelineRole                          m_Role = PipelineRole::Engine;
        std::future<void>                     m_Compile;
    };
} // namespace Desert::Graphic::API::Vulkan
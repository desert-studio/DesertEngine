#pragma once

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>

#include <vulkan/vulkan.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanPipelineCompute final : public ComputePipeline
    {
    public:
        VulkanPipelineCompute( const ComputePipelineSpecification& spec );
        ~VulkanPipelineCompute() override;

        [[nodiscard]] virtual PipelineType GetType() const override { return PipelineType::Compute; }
        [[nodiscard]] virtual const std::shared_ptr<Shader>& GetShader() const override { return m_Specification.Shader; }

        [[nodiscard]] virtual const ComputePipelineSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        // --- Resource-binding API (see ComputePipeline) ---
        ComputePipeline& SetInput( uint32_t binding, Image* image, RDG::Access declared,
                                   RDG::SubresourceRange range ) override;
        ComputePipeline& SetSampler( uint32_t binding, const RDG::SamplerDesc& desc ) override;
        ComputePipeline& SetOutput( uint32_t binding, Image* image, uint32_t mip = 0 ) override;
        ComputePipeline& SetStorageBuffer( uint32_t binding, ShaderResources::StorageBuffer* buffer ) override;
        ComputePipeline& SetPushConstants( const void* data, uint32_t size ) override;
        void             Dispatch( uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ ) override;
        void             Record( GpuBatch& batch, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ ) override;

        virtual void Invalidate() override;
        virtual void Release() override;

        VkPipeline GetVkPipeline() const
        {
            return m_ComputePipeline;
        }

        VkPipelineLayout GetVkPipelineLayout() const
        {
            return m_ComputePipelineLayout;
        }

        auto GetCommandBuffer() const
        {
            return m_ActiveComputeCommandBuffer;
        }

        void BindDescriptorSets( VkDescriptorSet descriptorSet, uint32_t frameIndex );
        void UpdateDescriptorSet( uint32_t frameIndex, const std::vector<VkWriteDescriptorSet>& writes,
                                  VkDescriptorSet descriptorSet, uint32_t setIndex = 0 );

        // The descriptor set layouts this pipeline's layout was built from, one per set.
        [[nodiscard]] const std::vector<DescriptorSetLayoutRef>& GetLayouts() const
        {
            return m_Layouts;
        }
        // The set-0 bindings the pipeline's own setters (SetInput / SetSampler / SetOutput / SetStorageBuffer)
        // fill: the "other route" of an in-graph dispatch (Renderer::DispatchCompute), sorted and unique.
        [[nodiscard]] std::vector<uint32_t>      GetBoundBindings() const;
        [[nodiscard]] std::span<const std::byte> GetBoundPushConstants() const
        {
            return m_BoundPushConstants;
        }
        // ROUTEFILL (UE: the parameter layout is made once and only read per pass). The other-route fill of the
        // bound bindings above as shader names (Renderer::GetPipelineRouteFill): kept, and rebuilt only when the
        // set of bound binding keys, the presence of push constants or the shader's slot table (a hot reload)
        // changes. A pass's setup shares the kept object; a rebind of the same binding (the GDF composite rebinds
        // its buffers every frame) changes nothing. Safe from several declaring threads.
        [[nodiscard]] std::shared_ptr<const RDG::OtherRouteFill> GetRouteFill() const;
        // Writes the resources the setters bound into @p descriptorSet (set 0). Refused, naming the pipeline and
        // the binding, when a bound resource has no view or copy to write.
        Common::BoolResultStr WriteBoundResources( VkDescriptorSet descriptorSet );

        VulkanMaterialBackend* GetVulkanMaterialBackend() const
        {
            return m_VulkanMaterialBackend.get();
        }

    private:
        // Release's body: @p deferred parks the handles in the allocator's deletion queue.
        void ReleaseHandles( bool deferred );

        ComputePipelineSpecification m_Specification;
        VkPipeline                   m_ComputePipeline       = VK_NULL_HANDLE;
        VkPipelineLayout             m_ComputePipelineLayout = VK_NULL_HANDLE;
        // Pipeline cache is now the shared device-wide one (VulkanLogicalDevice::GetPipelineCache).

        std::unique_ptr<VulkanMaterialBackend> m_VulkanMaterialBackend;

        // THE layouts of this pipeline: the ones m_ComputePipelineLayout was built from, the ones the
        // batch ring is allocated from, and the ones its pool is sized against. One capture, one
        // contract — a descriptor set and the pipeline layout it is bound to cannot describe different
        // shaders while both come from here. Strong references, so a shader recompile cannot pull them
        // out from under a pipeline that is still using them.
        std::vector<DescriptorSetLayoutRef> m_Layouts;
        uint64_t                            m_ShaderGeneration = 0;

        VkCommandBuffer m_ActiveComputeCommandBuffer = nullptr;

        // Bound resources for the next Dispatch (set via SetInput/SetOutput/SetPushConstants).
        struct OutputBinding
        {
            Image*   Image = nullptr;
            uint32_t Mip   = 0;
        };
        // A sampled input: the view of the subresource the caller declared (Range), in the layout its declared
        // access leaves the image in. There is no binding in the image's recorded layout.
        struct InputBinding
        {
            Image*                Image    = nullptr;
            RDG::Access           Declared = RDG::Access::SampledCompute;
            RDG::SubresourceRange Range    = RDG::SubresourceRange::All();
        };
        std::unordered_map<uint32_t, InputBinding>                    m_BoundInputs;
        std::unordered_map<uint32_t, OutputBinding>                   m_BoundOutputs;
        std::unordered_map<uint32_t, RDG::SamplerDesc>                m_BoundSamplers;
        std::unordered_map<uint32_t, ShaderResources::StorageBuffer*> m_BoundStorageBuffers;
        std::vector<std::byte>                                        m_BoundPushConstants;
        // Raised when a setter adds a binding key no map held, or push constants appear / disappear: the only
        // changes of what GetRouteFill is made of on the setter side (no setter removes a key).
        uint64_t m_BoundKeysVersion = 0;

        // The fill GetRouteFill hands out and what it was made from (the slot table is held, so its address
        // cannot come back for another table while it is compared).
        struct RouteFillCache
        {
            std::shared_ptr<const RdgSlotTable>        Slots;
            uint64_t                                   BoundKeysVersion = ~uint64_t{ 0 };
            std::shared_ptr<const RDG::OtherRouteFill> Fill;
        };
        mutable std::mutex     m_RouteFillLock;
        mutable RouteFillCache m_RouteFill;

        // Records the currently-bound resources into @p cmd against @p descriptorSet and dispatches.
        // Shared by the immediate Dispatch() and the batched Record(); performs no layout transitions and no
        // submission of its own.
        void RecordDescriptorsAndDispatch( VkCommandBuffer cmd, VkDescriptorSet descriptorSet,
                                           uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ );

        // Outputs to GENERAL, the dispatch, outputs back to SHADER_READ — the body both Dispatch (persistent
        // set, own buffer, waited) and Record (ring set, the batch's buffer) share.
        void RecordTransitionedDispatch( VkCommandBuffer cmd, VkDescriptorSet descriptorSet, uint32_t groupsX,
                                         uint32_t groupsY, uint32_t groupsZ );

        // Lazily-created ring of descriptor sets for the dispatches of a GpuBatch (Record), one per dispatch so
        // the batch's pending dispatches never alias one set; reused round-robin. In-graph dispatches do not
        // use it: Renderer::DispatchCompute writes the pass's own sets.
        void                         EnsureBatchRing();
        VkDescriptorPool             m_BatchPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_BatchRing;
        uint32_t                     m_BatchCursor  = 0;
        static constexpr uint32_t    kBatchRingSize = 64;
    };
} // namespace Desert::Graphic::API::Vulkan
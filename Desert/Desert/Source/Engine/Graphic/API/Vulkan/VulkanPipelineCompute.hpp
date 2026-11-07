#pragma once

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>

#include <vulkan/vulkan.hpp>

#include <Common/Core/ResultStr.hpp>

#include <atomic>
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

        // EVERYTHING ONE Invalidate PRODUCES, immutable once published (UE: a compute PSO is never edited after
        // it is built; the replaced one goes to deferred deletion). The pipeline, the layout made from Layouts,
        // the material backend whose sets match Layouts, and the batch ring allocated from Layouts are ONE
        // record, so a reader that took the record once binds a pipeline, a layout and sets of one contract
        // even if the game thread rebuilds this pipeline meanwhile. The replaced record is retired through
        // VulkanAllocator::RT_DestroyLater: it stays valid for the frames in flight.
        struct BuiltCompute
        {
            BuiltCompute()                                 = default;
            BuiltCompute( const BuiltCompute& )            = delete;
            BuiltCompute& operator=( const BuiltCompute& ) = delete;
            ~BuiltCompute();

            VkPipeline                          Pipeline = VK_NULL_HANDLE;
            VkPipelineLayout                    Layout   = VK_NULL_HANDLE;
            std::vector<DescriptorSetLayoutRef> Layouts;
            uint64_t                            ShaderGeneration = 0;
            // Co-owned: a rebuild against an unchanged shader generation shares it into the next record.
            std::shared_ptr<VulkanMaterialBackend> Backend;

            // The next set of the lazily-created ring for the dispatches of a GpuBatch (Record), one per
            // dispatch so the batch's pending dispatches never alias one set; reused round-robin. Allocated
            // from THIS record's Layouts, never the shader's current ones. VK_NULL_HANDLE with no layouts.
            [[nodiscard]] VkDescriptorSet NextBatchSet() const noexcept;

        private:
            static constexpr uint32_t            kBatchRingSize = 64;
            mutable std::mutex                   m_BatchLock;
            mutable VkDescriptorPool             m_BatchPool = VK_NULL_HANDLE;
            mutable std::vector<VkDescriptorSet> m_BatchRing;
            mutable uint32_t                     m_BatchCursor = 0;
        };

        // THE ONE READ of the published record: pipeline, layout, set layouts and backend of one build. Valid
        // for the frames in flight (a rebuild retires it, never frees it at once). Never null.
        [[nodiscard]] const BuiltCompute& GetBuilt() const noexcept
        {
            return *m_Current.load( std::memory_order_acquire );
        }

        VkPipeline GetVkPipeline() const
        {
            return GetBuilt().Pipeline;
        }

        VkPipelineLayout GetVkPipelineLayout() const
        {
            return GetBuilt().Layout;
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
            return GetBuilt().Layouts;
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
        // Writes the resources the setters bound into @p descriptorSet (set 0) of @p built (the record the caller
        // binds). Refused, naming the pipeline and the binding, when a bound resource has no view or copy to
        // write.
        Common::BoolResultStr WriteBoundResources( const BuiltCompute& built, VkDescriptorSet descriptorSet );

        VulkanMaterialBackend* GetVulkanMaterialBackend() const
        {
            return GetBuilt().Backend.get();
        }

    private:
        // Release's body: publishes a record with no handles (the backend kept); @p deferred retires the
        // replaced record through the allocator's deletion queue, otherwise it dies at once.
        void ReleaseHandles( bool deferred );
        // Makes @p next THE record readers see; the replaced one is retired (deferred) or dropped at once.
        void Publish( std::shared_ptr<const BuiltCompute> next, bool deferred ) noexcept;

        ComputePipelineSpecification m_Specification;
        // Pipeline cache is the shared device-wide one (VulkanLogicalDevice::GetPipelineCache).

        // The published record (owned here, written by the game thread only) and the pointer readers load. The
        // layouts in it are strong references, so a shader recompile cannot pull them out from under a
        // pipeline that is still using them, and the pipeline layout, the backend's sets and the batch ring
        // all come from them: a set and the pipeline layout it is bound to cannot describe different shaders.
        std::shared_ptr<const BuiltCompute> m_Owned;
        std::atomic<const BuiltCompute*>    m_Current{ nullptr };

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
        void RecordDescriptorsAndDispatch( const BuiltCompute& built, VkCommandBuffer cmd,
                                           VkDescriptorSet descriptorSet, uint32_t groupsX, uint32_t groupsY,
                                           uint32_t groupsZ );

        // Outputs to GENERAL, the dispatch, outputs back to SHADER_READ — the body both Dispatch (persistent
        // set, own buffer, waited) and Record (ring set, the batch's buffer) share. In-graph dispatches use
        // neither: Renderer::DispatchCompute writes the pass's own sets.
        void RecordTransitionedDispatch( const BuiltCompute& built, VkCommandBuffer cmd,
                                         VkDescriptorSet descriptorSet, uint32_t groupsX, uint32_t groupsY,
                                         uint32_t groupsZ );
    };
} // namespace Desert::Graphic::API::Vulkan
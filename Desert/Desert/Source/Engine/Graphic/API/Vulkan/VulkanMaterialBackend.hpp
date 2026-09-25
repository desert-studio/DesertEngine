#pragma once

#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

#include <Engine/Graphic/ViewDescriptorSets.hpp>

#include <Engine/Graphic/Materials/MaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <unordered_set>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanMaterialBackend final : public MaterialBackend
    {
    public:
        VulkanMaterialBackend( const std::shared_ptr<Shader>& shader );
        ~VulkanMaterialBackend();

        virtual void ApplyUniformBuffer( MaterialProperty* prop ) override;
        virtual void ApplyStorageBuffer( MaterialProperty* prop ) override;
        virtual void ApplyTexture2D( MaterialProperty* prop ) override;
        virtual void ApplyTextureCube( MaterialProperty* prop ) override;

        virtual void FlushUpdates() override;

        // The ACTIVE view's set, made (fallbacks, then image seeds) the first time that view asks for it;
        // VK_NULL_HANDLE when it could not be made (named once in the log).
        VkDescriptorSet GetDescriptorSet( uint32_t frameIndex, uint32_t setIndex = 0 );

        void UpdateDescriptorSets( const std::vector<VkWriteDescriptorSet>& writes );

        // False when the active view's sets could not be made: the draw must not run, or it would sample
        // whatever the previous material left bound.
        [[nodiscard]] bool BindDescriptorSets( VkCommandBuffer cmdBuffer, VkPipelineLayout layout,
                                               VkPipelineBindPoint bindPoint, uint32_t frameIndex );

        // True when the shader declares descriptor resources. The sets themselves are made per view, on
        // that view's first use (see Graphic::ViewDescriptorSets).
        bool HasDescriptorSets() const;

        /** The layouts this backend's descriptor sets were allocated from, captured at construction. */
        const std::vector<DescriptorSetLayoutRef>& GetLayouts() const
        {
            return m_Layouts;
        }

        /** The shader's reload generation at the moment those sets were allocated. */
        uint32_t GetShaderGeneration() const
        {
            return m_ShaderGeneration;
        }

    private:
        // Points every declared binding of the active view's new sets for @p frameIndex at a fallback, so no
        // set is ever bound with an undefined descriptor.
        void WriteFallbacks( uint32_t frameIndex );

        // The active view's sets for @p frameIndex, made on first use; null (named once) on failure.
        IViewDescriptorSetCopy* ActiveSets( uint32_t frameIndex );

        // Says so, once, if the shader has been recompiled into a DIFFERENT SHAPE since these sets were
        // allocated. Silent when the recompile kept the same bindings, because that case is genuinely
        // fine — Vulkan compares set layouts by content.
        void ReportShapeDriftOnce();

        std::shared_ptr<VulkanShader> m_VulkanShader; // TODO: weak ptr

        // The layouts every set below was allocated from, held for as long as those sets exist. Not
        // re-read from the shader: a recompile publishes new layouts, and a set allocated from the old
        // one has to keep the old one alive to stay legal.
        std::vector<DescriptorSetLayoutRef> m_Layouts;
        uint32_t                            m_ShaderGeneration   = 0;
        bool                                m_ShapeDriftReported = false;

        // Per VIEW and frame in flight, made lazily: a view that never draws this material pays nothing,
        // and one opened later gets fresh sets (fallbacks, then the seeded images) instead of inheriting a
        // renderer slot's previous owner's. The swallow bookkeeping lives in each view's copy.
        ViewDescriptorSets m_ViewSets;
        bool               m_ResolveFailureReported = false;

        // Bindings whose "no copy to bind" refusal was already logged (once per material lifetime).
        std::unordered_set<uint32_t> m_BindRefusalReported;

        // Bindings whose swallowed rebind was already reported, so a per-draw defect logs once per
        // material lifetime instead of once per draw per frame.
        std::unordered_set<uint32_t> m_SwallowReported;

        // Success path: remember what @p binding was just given in these sets this frame.
        void NoteDescriptorWrite( IViewDescriptorSetCopy& sets, uint32_t binding, uint64_t handle );
        // Early-return path: if @p binding was written this frame with a DIFFERENT resource, say so
        // (once), naming the shader and both handles. @p what names the descriptor kind for the log.
        void ReportSwallowedRebind( const IViewDescriptorSetCopy& sets, uint32_t binding, uint64_t handle,
                                    const char* what );

        VkBuffer      m_DummyBuffer = VK_NULL_HANDLE;
        VmaAllocation m_DummyAllocation = nullptr;
    };
} // namespace Desert::Graphic::API::Vulkan
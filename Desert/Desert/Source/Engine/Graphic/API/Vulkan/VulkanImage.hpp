#pragma once

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFormat.hpp>

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <vulkan/vulkan.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    /**
     * @brief Internal POD structure for Vulkan image handles and state.
     */
    struct VulkanImageResource
    {
        VkImage               Image        = VK_NULL_HANDLE;
        VkImageView           ImageView    = VK_NULL_HANDLE;
        VkSampler             Sampler      = VK_NULL_HANDLE;
        VmaAllocation         Allocation   = nullptr;
        VkFormat              Format       = VK_FORMAT_UNDEFINED;
        VkImageLayout         Layout       = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t              MipLevels    = 1;
        uint32_t              LayerCount   = 1;
        // While a render graph has the subresources in different layouts (a mip chain mid-walk: the bloom
        // pyramid, a mip generation) no single layout is true: Layout is UNDEFINED and this holds each
        // subresource's layout, in RDG::TextureDesc::SubresourceIndex order (layer-major). Empty whenever every
        // subresource is in Layout.
        std::vector<VkImageLayout> SubresourceLayouts;

        // The layout subresource @p index (RDG::TextureDesc::SubresourceIndex) is in.
        [[nodiscard]] VkImageLayout LayoutOf( uint32_t index ) const
        {
            if ( Layout == VK_IMAGE_LAYOUT_UNDEFINED && index < SubresourceLayouts.size() )
                return SubresourceLayouts[index];
            return Layout;
        }

        // The render graph's write-back, one layout per subresource: one shared layout collapses to Layout.
        void RecordLayouts( std::vector<VkImageLayout> layouts )
        {
            if ( layouts.empty() )
                return;
            const bool uniform = std::all_of( layouts.begin(), layouts.end(),
                                              [&]( VkImageLayout layout ) { return layout == layouts.front(); } );
            if ( uniform )
            {
                Layout = layouts.front();
                SubresourceLayouts.clear();
                return;
            }
            Layout             = VK_IMAGE_LAYOUT_UNDEFINED;
            SubresourceLayouts = std::move( layouts );
        }

        VkDescriptorImageInfo GetDescriptorInfo() const
        {
            return { Sampler, ImageView, Layout };
        }
    };

    // Resolves the device-dependent depth entry and defers to Utils::GetVulkanFormat.
    VkFormat GetImageVulkanFormat( const Core::Formats::ImageFormat& format );

    /**
     * @brief Base interface for Vulkan-specific image operations.
     */
    class IVulkanImage
    {
    public:
        virtual ~IVulkanImage() = default;

        [[nodiscard]] virtual const VulkanImageResource& GetResource() const = 0;

        virtual void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, uint32_t mip = 0 ) = 0;

        // Explicitly-synchronized whole-image layout transition (proper src/dst stage + access masks,
        // unlike the conservative ALL_COMMANDS/zero-access default above). Needed for compute storage
        // targets where execution-only ordering is insufficient (cache flush/invalidate is required),
        // and for presenting the scene depth image to a compute sampler. Updates the tracked layout so
        // descriptor binds capture it. The aspect mask comes from the image's FORMAT, so a packed
        // depth+stencil image is transitioned as DEPTH|STENCIL rather than as colour.
        virtual void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout,
                                       VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                       VkAccessFlags srcAccess, VkAccessFlags dstAccess ) = 0;

        // The layout this image lives in when nobody is actively reading or writing it — GENERAL for a
        // storage image, DEPTH_STENCIL_ATTACHMENT_OPTIMAL for a depth format, SHADER_READ_ONLY_OPTIMAL
        // otherwise. A borrowed image (the scene depth) must be handed back in it.
        [[nodiscard]] virtual VkImageLayout GetDefaultLayout() const = 0;

        [[nodiscard]] virtual VkImageView GetMipView( uint32_t level ) const = 0;

        // Destroy + recreate this image's VkSampler from the current RenderConfig filter (live filter swap).
        virtual void RecreateSampler() = 0;

        // --- The render graph's import (VulkanRendererAPI::ImportImage): ONE path for every image kind ---
        // What the graph knows of this image: its kind (2D, a volume, a cube), extent (a volume's depth),
        // format, mips, layers and samples. Barrier ranges and views follow from it (VulkanRdgTexture).
        [[nodiscard]] virtual RDG::TextureDesc GetGraphDesc() const = 0;

        // The render graph's write-back after every barrier batch that touches this image: the layout each
        // subresource is in (RDG::TextureDesc::SubresourceIndex order), so TransitionLayout, descriptor binds
        // and the next import start from the truth, one mip at a time while a mip chain is mid-walk.
        virtual void RecordLayouts( std::vector<VkImageLayout> layouts ) = 0;

        // The graph's handle on this image, kept for the image's lifetime so the views and framebuffers the
        // graph builds on it are made once, not every frame. Dropped with the VkImage (each kind's Release).
        const std::shared_ptr<RDG::IPhysicalTexture>& GetGraphTexture() const
        {
            return m_GraphTexture;
        }
        void SetGraphTexture( std::shared_ptr<RDG::IPhysicalTexture> texture )
        {
            m_GraphTexture = std::move( texture );
        }

    protected:
        // Its views name the VkImage the caller is about to release (or resize). A frame in flight may still
        // reference them, so the handle is retired to the frame graph's pool (VulkanRdgPool::Retire), which
        // releases it once every such frame has completed; the VkImage itself waits in the allocator's frame
        // deletion queue the same way (RT_DestroyImage).
        void DropGraphTexture();

    private:
        std::shared_ptr<RDG::IPhysicalTexture> m_GraphTexture;
    };

    /**
     * @brief implementation of a 2D Vulkan Image.
     */
    class VulkanImage2D final : public Image2D, public IVulkanImage
    {
    public:
        explicit VulkanImage2D( const Core::Formats::Image2DSpecification& spec );
        ~VulkanImage2D() override;

        // --- Image2D Interface ---
        [[nodiscard]] uint32_t GetWidth() const override { return m_Specification.Width; }
        [[nodiscard]] uint32_t GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] uint32_t GetMipmapLevels() const override
        {
            return m_Resource.MipLevels;
        }
        [[nodiscard]] Core::Formats::Image2DSpecification& GetImageSpecification() override
        {
            return m_Specification;
        }
        NO_DISCARD Common::ResultStr<std::vector<uint8_t>> ReadPixelsRGBA8() override;
        NO_DISCARD Common::ResultStr<std::shared_ptr<ImageReadback>> BeginReadbackRGBA8() override;

        Common::BoolResultStr Invalidate() override;
        Common::BoolResultStr Release() override;
        NO_DISCARD Common::BoolResultStr SetData( const Core::Formats::ImagePixelData& data ) override;

        // --- IVulkanImage Interface ---
        [[nodiscard]] const VulkanImageResource& GetResource() const override { return m_Resource; }
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, uint32_t mip = 0 ) override;
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                               VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                               VkAccessFlags dstAccess ) override;
        [[nodiscard]] VkImageLayout GetDefaultLayout() const override;
        [[nodiscard]] VkImageView   GetMipView( uint32_t level ) const override;
        void                        RecreateSampler() override;

        // --- Vulkan Specific ---
        NO_DISCARD Common::BoolResultStr RT_Invalidate();

        [[nodiscard]] RDG::TextureDesc GetGraphDesc() const override
        {
            RDG::TextureDesc desc;
            desc.Size    = { m_Specification.Width, m_Specification.Height, 1 };
            desc.Format  = m_Specification.Format;
            desc.Mips    = m_Resource.MipLevels;
            desc.Layers  = m_Resource.LayerCount;
            desc.Dim     = RDG::TextureDim::Tex2D;
            desc.Samples = std::max( 1u, m_Specification.Samples );
            return desc;
        }
        void RecordLayouts( std::vector<VkImageLayout> layouts ) override
        {
            m_Resource.RecordLayouts( std::move( layouts ) );
        }

    private:
        Common::BoolResultStr CreateResource();
        void UploadData( VkCommandBuffer cmdBuffer, VkBuffer stagingBuffer );
        // Generate mips 1..N-1 from mip 0 via linear blits, leaving every mip in `finalLayout`.
        // Precondition: the whole image is in TRANSFER_DST_OPTIMAL and mip 0 holds the source pixels.
        void GenerateMips( VkCommandBuffer cmdBuffer, VkImageLayout finalLayout );

    private:
        Core::Formats::Image2DSpecification m_Specification;
        VulkanImageResource                 m_Resource;
        std::vector<VkImageView>            m_MipViews;
        bool                                m_IsLoaded = false;
    };

    /**
     * @brief implementation of a Cubemap Vulkan Image.
     */
    class VulkanImageCube final : public ImageCube, public IVulkanImage
    {
    public:
        explicit VulkanImageCube( const Core::Formats::ImageCubeSpecification& spec );
        ~VulkanImageCube() override;

        // --- ImageCube Interface ---
        // A cube's faces are square: both extents ARE the face. The spec names the face directly
        // (no 4x3-cross arithmetic — see ImageCubeSpecification::FaceSize for the defects that bought).
        [[nodiscard]] uint32_t GetWidth() const override
        {
            return m_Specification.FaceSize;
        }
        [[nodiscard]] uint32_t GetHeight() const override
        {
            return m_Specification.FaceSize;
        }
        [[nodiscard]] uint32_t GetMipmapLevels() const override
        {
            return m_Resource.MipLevels;
        }
        [[nodiscard]] Core::Formats::ImageCubeSpecification& GetImageSpecification() override
        {
            return m_Specification;
        }

        Common::BoolResultStr Invalidate() override;
        Common::BoolResultStr Release() override;

        // --- IVulkanImage Interface ---
        [[nodiscard]] const VulkanImageResource& GetResource() const override { return m_Resource; }
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, uint32_t mip = 0 ) override;
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                               VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                               VkAccessFlags dstAccess ) override;
        [[nodiscard]] VkImageLayout GetDefaultLayout() const override;
        [[nodiscard]] VkImageView GetMipView( uint32_t level ) const override;
        void RecreateSampler() override;
        [[nodiscard]] RDG::TextureDesc GetGraphDesc() const override
        {
            RDG::TextureDesc desc;
            desc.Size   = { m_Specification.FaceSize, m_Specification.FaceSize, 1 };
            desc.Format = m_Specification.Format;
            desc.Mips   = m_Resource.MipLevels;
            desc.Layers = m_Resource.LayerCount;
            desc.Dim    = RDG::TextureDim::Cube;
            return desc;
        }
        void RecordLayouts( std::vector<VkImageLayout> layouts ) override
        {
            m_Resource.RecordLayouts( std::move( layouts ) );
        }

        // --- Vulkan Specific ---
        NO_DISCARD Common::BoolResultStr RT_Invalidate();

        /// Writes one colour into every texel of every face and mip, immediately.
        ///
        /// WHY A CUBE NEEDS THIS AND A 2D DOES NOT. `UploadData` is a no-op for cubes, so the pixels a
        /// caller puts in `ImageCubeSpecification::Data` never reach the device and a freshly created cube
        /// holds whatever the allocator handed out. That is tolerable for a cube a compute pass is about
        /// to fill, and it is NOT tolerable for the one cube whose CONTENT is a statement — the fallback a
        /// binding points at when the scene has no environment (Г14). A statement made out of undefined
        /// memory is not a statement.
        NO_DISCARD Common::BoolResultStr RT_ClearToColor( float r, float g, float b, float a );

        /// Submits a copy of every level of every face, in the image's OWN format, tightly packed in table
        /// order (level 0's six faces, then level 1's), and does not wait: poll the readback, then
        /// ReadBytes() — on any thread. See the definition for why it converts nothing.
        NO_DISCARD Common::ResultStr<std::shared_ptr<ImageReadback>> RT_BeginReadAllLevels();

    private:
        Common::BoolResultStr CreateResource();
        void UploadData( VkCommandBuffer cmdBuffer, VkBuffer stagingBuffer );

    private:
        Core::Formats::ImageCubeSpecification m_Specification;
        VulkanImageResource                   m_Resource;
        std::vector<VkImageView>              m_MipViews;
    };

    /**
     * @brief implementation of a 3D Vulkan Image (VK_IMAGE_TYPE_3D / VK_IMAGE_VIEW_TYPE_3D).
     *
     * One mip level, one array layer: a volume is either uploaded whole from CPU pixels or filled by a
     * compute dispatch writing `image3D`, and neither has a chain to build. Its sampler is LINEAR /
     * REPEAT unconditionally — see the note on Graphic::Image3D.
     */
    class VulkanImage3D final : public Image3D, public IVulkanImage
    {
    public:
        explicit VulkanImage3D( const Core::Formats::Image3DSpecification& spec );
        ~VulkanImage3D() override;

        // --- Image3D Interface ---
        [[nodiscard]] uint32_t GetWidth() const override
        {
            return m_Specification.Width;
        }
        [[nodiscard]] uint32_t GetHeight() const override
        {
            return m_Specification.Height;
        }
        [[nodiscard]] uint32_t GetMipmapLevels() const override
        {
            return m_Resource.MipLevels;
        }
        [[nodiscard]] Core::Formats::Image3DSpecification& GetImageSpecification() override
        {
            return m_Specification;
        }

        Common::BoolResultStr Invalidate() override;
        Common::BoolResultStr Release() override;

        // --- IVulkanImage Interface ---
        [[nodiscard]] const VulkanImageResource& GetResource() const override
        {
            return m_Resource;
        }
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, uint32_t mip = 0 ) override;
        void TransitionLayout( VkCommandBuffer cmdBuffer, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                               VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                               VkAccessFlags dstAccess ) override;
        [[nodiscard]] VkImageLayout GetDefaultLayout() const override;
        [[nodiscard]] VkImageView   GetMipView( uint32_t level ) const override;
        void                        RecreateSampler() override;
        // A volume: one layer, the mip chain spans all three extents, so a barrier's subresource range is
        // [mips) x layer 0 and the graph's view of it is VK_IMAGE_VIEW_TYPE_3D.
        [[nodiscard]] RDG::TextureDesc GetGraphDesc() const override
        {
            RDG::TextureDesc desc;
            desc.Size   = { m_Specification.Width, m_Specification.Height, m_Specification.Depth };
            desc.Format = m_Specification.Format;
            desc.Mips   = m_Resource.MipLevels;
            desc.Layers = 1;
            desc.Dim    = RDG::TextureDim::Tex3D;
            return desc;
        }
        void RecordLayouts( std::vector<VkImageLayout> layouts ) override
        {
            m_Resource.RecordLayouts( std::move( layouts ) );
        }

        // --- Vulkan Specific ---
        NO_DISCARD Common::BoolResultStr RT_Invalidate();

    private:
        Common::BoolResultStr CreateResource();

    private:
        Core::Formats::Image3DSpecification m_Specification;
        VulkanImageResource                 m_Resource;
        // Kept as a vector purely so RT_DestroyImage's deferred-deletion entry takes the same shape as
        // the 2D and cube paths; a volume has exactly one view in it.
        std::vector<VkImageView> m_MipViews;
    };

} // namespace Desert::Graphic::API::Vulkan

#pragma once

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <vulkan/vulkan.h>
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

        VkDescriptorImageInfo GetDescriptorInfo() const
        {
            return { Sampler, ImageView, Layout };
        }
    };

    namespace Utils
    {
        // The engine format -> VkFormat table. Total over ImageFormat, no `default:` label and nothing
        // returned after the switch, for the same reason as Core::Formats::GetBytesPerPixel: the previous
        // version fell through to VK_FORMAT_UNDEFINED for anything it did not list, and an image created
        // with an undefined format is a problem you meet in a capture, not at the line that caused it.
        // Constexpr so the guard below can constant-evaluate it — see the long note in
        // Core/Formats/ImageFormat.hpp for why a warning cannot do this job in this workspace.
        //
        // @p deviceDepthFormat is the packed depth+stencil format the physical device selected; it is the
        // only entry that is not a compile-time constant.
        constexpr VkFormat GetVulkanFormat( Core::Formats::ImageFormat format, VkFormat deviceDepthFormat )
        {
            switch ( format )
            {
                case Core::Formats::ImageFormat::RGBA8F:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Core::Formats::ImageFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Core::Formats::ImageFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case Core::Formats::ImageFormat::BGRA8F:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case Core::Formats::ImageFormat::DEPTH32F:
                    return VK_FORMAT_D32_SFLOAT;
                case Core::Formats::ImageFormat::DEPTH24STENCIL8:
                    return deviceDepthFormat;
                // The four block formats. `textureCompressionBC` is read off the physical device and
                // requested by `VulkanLogicalDevice::CreateDevice` (pinned by the TextureCompressionBC
                // suite); without it these two VkFormats are not usable, and on THIS machine that is
                // undetectable at run time — measured, four mode-6 blocks came back bit-exact with the
                // feature off and no validation message. The census is the gate, not the device.
                case Core::Formats::ImageFormat::BC7_UNORM:
                    return VK_FORMAT_BC7_UNORM_BLOCK;
                case Core::Formats::ImageFormat::BC6H_UFLOAT:
                    return VK_FORMAT_BC6H_UFLOAT_BLOCK;
                // UNORM and not SNORM for both. The signed variants are different VkFormats with a
                // different endpoint transform, and the engine's normal maps arrive as UNORM bytes that
                // the shader re-centres with `2*n - 1`; picking the signed format here would move that
                // re-centring into the hardware for BC5 and leave it in the shader for RGBA8, so one of
                // the two paths would be wrong and only on the textures an author had marked up.
                case Core::Formats::ImageFormat::BC4_UNORM:
                    return VK_FORMAT_BC4_UNORM_BLOCK;
                case Core::Formats::ImageFormat::BC5_UNORM:
                    return VK_FORMAT_BC5_UNORM_BLOCK;
                case Core::Formats::ImageFormat::Count:
                    break; // the sentinel is not a format — fall through to the error path below
            }

            LOG_ERROR( "GetVulkanFormat: ImageFormat value {} is outside the enumeration",
                       static_cast<uint32_t>( format ) );
            DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
        }

        namespace Detail
        {
            constexpr bool VulkanFormatTableIsTotal()
            {
                for ( uint32_t i = 0; i < Core::Formats::kImageFormatCount; ++i )
                {
                    if ( GetVulkanFormat( static_cast<Core::Formats::ImageFormat>( i ),
                                          VK_FORMAT_D32_SFLOAT_S8_UINT ) == VK_FORMAT_UNDEFINED )
                        return false;
                }
                return true;
            }
        } // namespace Detail

        static_assert( Detail::VulkanFormatTableIsTotal(),
                       "Every ImageFormat enumerator needs a VkFormat in Utils::GetVulkanFormat." );
    } // namespace Utils

    // Resolves the device-dependent depth entry and defers to Utils::GetVulkanFormat.
    VkFormat GetImageVulkanFormat( const Core::Formats::ImageFormat& format );

    // Aspect mask a barrier or a view must name for this format. Translates the backend-independent
    // Core::Formats::GetImageAspect answer into Vulkan bits explicitly, rather than assuming the two
    // enumerations happen to share numeric values.
    VkImageAspectFlags GetImageVulkanAspect( Core::Formats::ImageFormat format );

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

        /// Every level of every face, in the image's OWN format, tightly packed in table order
        /// (level 0's six faces, then level 1's). See the definition for why it converts nothing.
        NO_DISCARD Common::ResultStr<std::vector<unsigned char>> RT_ReadAllLevels();

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

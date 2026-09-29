#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Engine/Graphic/DynamicResources.hpp>

namespace Desert::Graphic
{
    enum class AttachmentLoad : uint8_t
    {
        Clear    = 0,
        Load     = 1,
        DontCare = 2
    };

    enum class AttachmentStore : uint8_t
    {
        Store    = 0,
        DontCare = 1
    };

    struct FramebufferAttachment
    {
        Core::Formats::ImageFormat Format  = Core::Formats::ImageFormat::RGBA8F;
        AttachmentLoad             LoadOp  = AttachmentLoad::Clear;
        AttachmentStore            StoreOp = AttachmentStore::Store;

        FramebufferAttachment() = default;
        FramebufferAttachment( Core::Formats::ImageFormat format,
                               AttachmentLoad             load  = AttachmentLoad::Clear,
                               AttachmentStore            store = AttachmentStore::Store )
             : Format( format ), LoadOp( load ), StoreOp( store )
        {
        }
    };

    struct FramebufferAttachmentSpecification
    {
        FramebufferAttachmentSpecification() = default;
        FramebufferAttachmentSpecification( const std::initializer_list<Core::Formats::ImageFormat>& formats )
        {
            for ( auto f : formats )
                Attachments.emplace_back( f );
        }

        std::vector<FramebufferAttachment> Attachments;
    };

    class Framebuffer;

    struct ExternalAttachment
    {
        std::shared_ptr<Framebuffer> SourceFramebuffer;
        uint32_t                     AttachmentIndex = 0;
        AttachmentLoad               Load            = AttachmentLoad::Clear;
    };

    struct ExternalFramebuffer
    {
        std::vector<ExternalAttachment> ColorAttachments;
        std::optional<ExternalAttachment> DepthAttachment;
    };

    struct FramebufferSpecification
    {
        uint32_t                           Width   = 1280;
        uint32_t                           Height  = 720;
        // MSAA sample count (1 = off, 2/4/8...). When > 1 the Vulkan framebuffer renders into
        // multisampled attachments and RESOLVES each color into a single-sample image at
        // subpass end; GetColorAttachmentImage() returns the RESOLVED image, so downstream
        // passes (post stack, ImGui viewport) are untouched. NOTE: the field was previously an
        // unused "2" — it is honoured now, so the default must be OFF.
        uint32_t                           Samples = 1;
        FramebufferAttachmentSpecification Attachments;
        std::string                        DebugName;
        bool                               NoResizeble = false;

        // Swapchain-present wrapper: build the render pass with the swapchain's simple color subpass
        // dependency (src COLOR_OUTPUT/access 0, dst COLOR_OUTPUT/WRITE) instead of the offscreen one, so
        // pipelines built against this framebuffer are render-pass-compatible with the actual present pass.
        bool PresentTarget = false;

        ExternalFramebuffer ExternalAttachments;
    };

    class Framebuffer : public DynamicResources
    {
    public:
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp. A framebuffer's ATTACHMENTS are Images
        // and carry rows of their own; this row is the `VkFramebuffer` + its render passes.
        Framebuffer() : m_Accounting( ResourceOwnership::Take( ResourceKind::Framebuffer ) )
        {
        }

        virtual ~Framebuffer() = default;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        virtual const FramebufferSpecification GetSpecification() const = 0;

        // NO `forceRecreate`. The parameter was here with a default of `false`, every one of the ten call
        // sites but one took the default (the one that did not asked for `true`), and the only
        // implementation ignored it and recreated unconditionally —
        // so "force" named the sole behaviour there has ever been. `-Wunused-parameter` found it in
        // VulkanFramebuffer::Resize.
        virtual Common::BoolResultStr Resize( uint32_t width, uint32_t height ) = 0;

        virtual uint32_t GetFramebufferWidth() const  = 0;
        virtual uint32_t GetFramebufferHeight() const = 0;

        virtual uint32_t GetColorAttachmentCount() const = 0;
        virtual uint32_t GetDepthAttachmentCount() const = 0;

        // Observe-only: return the stored attachment by const-ref (no atomic refcount bump per call —
        // these are hit per-frame per-pass). Callers that need ownership copy explicitly. See the
        // ownership convention ([[cpp-ownership-convention]]).
        virtual const std::shared_ptr<Image2D>& GetColorAttachmentImage( uint32_t index = 0 ) const = 0;
        virtual const std::shared_ptr<Image2D>& GetDepthAttachmentImage() const                     = 0;
        // Samples > 1 only: the multisampled image colour @p index renders into. GetColorAttachmentImage returns
        // the single-sample image it resolves into, which is what every reader samples.
        virtual const std::shared_ptr<Image2D>& GetMultisampleColorAttachmentImage( uint32_t index ) const = 0;

        static std::shared_ptr<Framebuffer> Create( const FramebufferSpecification& spec );

    private:
        ResourceOwnership m_Accounting;
    };

    class FramebufferLibrary final
    {
    public:
        static const auto& GetFramebuffers()
        {
            return s_Framebuffers;
        }

    private:
        static inline std::vector<std::shared_ptr<Graphic::Framebuffer>> s_Framebuffers;

        friend class Framebuffer;
    };
} // namespace Desert::Graphic
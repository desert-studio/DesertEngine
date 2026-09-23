#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/DynamicResources.hpp>

#include <Engine/Graphic/MipMapGenerator.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <Common/Core/UUID.hpp>

namespace Desert::Graphic
{
    class Image2D;
    class Image3D;

    using ImageCubeRef = std::shared_ptr<ImageCube>;
    using Image2DRef   = std::shared_ptr<Image2D>;
    using Image3DRef   = std::shared_ptr<Image3D>;

    class Image
    {
    public:
        // A runtime-only identity: an Image has no path to derive one from, and two images must never
        // share a key in the descriptor caches.
        //
        // EVERY IMAGE IS IN THE LEDGER FROM HERE, whatever backend made it and whoever ends up holding it.
        // This constructor is the only place the engine sees all of them: `ImageService` looked like that
        // place and is not — its Register() is reached from eight call sites, while a renderer LUT, a
        // framebuffer attachment, a font atlas and a panel slice are built by four different files.
        //
        // AND `Image2D::Create` IS NOT THAT PLACE EITHER, which is what this paragraph used to say. Seven
        // sites — two in VulkanFramebuffer.cpp, five in VulkanFallbackTextures.cpp — construct the backend
        // image directly, so they reach THIS constructor and never reach that factory. The
        // difference was invisible while the row was only a count and cost 320 MiB of shadow cascades out
        // of the ledger's byte total the day it started carrying one; the size is therefore recorded
        // beside the allocation now. The
        // KIND comes from the subclass because a base cannot know it; the OWNER does not, because a
        // constructor cannot know who is about to hold the pointer — see ResourceLedger.hpp on why
        // "Unclaimed" is a reportable answer rather than a default bucket.
        explicit Image( const ResourceKind kind )
             : m_Hash( Common::UUID::Generate() ), m_Accounting( ResourceOwnership::Take( kind ) )
        {
        }

        virtual ~Image() = default;

        // NAME THE OPERATION; THE TOKEN ITSELF IS NOT REACHABLE. Handing out a `ResourceOwnership&` would
        // let a caller move it out and leave the image alive with no row — the one failure this ledger's
        // whole shape exists to make unwritable (ResourceLedger.hpp, "why a type rather than a pair of
        // calls"). Same move as MappedMemory: there is no Data(), so the wrong call does not compile.
        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        /// What this image costs on the device, once the backend knows. Ignored when 0.
        void RecordDeviceBytes( const std::size_t bytes )
        {
            m_Accounting.RecordBytes( bytes );
        }

        [[nodiscard]] ResourceOwner GetResourceOwner() const
        {
            return m_Accounting.GetOwner();
        }

        [[nodiscard]] Common::AssetHandle GetOwningAsset() const
        {
            return m_Accounting.GetAsset();
        }

        virtual uint32_t GetWidth() const        = 0;
        virtual uint32_t GetHeight() const       = 0;
        virtual uint32_t GetMipmapLevels() const = 0;

        // GetImageFormat / IsLoaded / GetImagePixels USED TO SIT HERE and Г12 removed all three, because
        // a dead pure virtual is not inert: it is a standing instruction to every future implementer to
        // write a body, and the three bodies these already had are the argument. GetImagePixels was
        // implemented as `DESERT_VERIFY(false)` on Image2D, as an abort on Image3D — and on ImageCube as
        // a bare `return {}`, which is §1.4 of the contract sitting armed, waiting for its first caller
        // to read "the cubemap is blank" out of "nobody implemented this". Format and loaded-ness are
        // read off the specification, which is where they are authored.
        //
        // CPU readback is Image2D::ReadPixelsRGBA8 below, and it is deliberately the ONLY one: a volume
        // is produced on the GPU and consumed on the GPU, so a 3D readback would be a 32 MiB stall
        // written for nobody. That reason is kept here because it is the reason there is no general
        // GetImagePixels, not a note about a function that no longer exists.

        virtual const Common::UUID GetHash() const final
        {
            return m_Hash;
        }

        // Byte size and bytes-per-pixel now live with the format they describe, as total functions:
        // Core::Formats::GetBytesPerPixel / CalculateImageSize (Core/Formats/ImageFormat.hpp). They used
        // to be statics here that returned 0 for any format they did not recognise.

    private:
        const Common::UUID m_Hash;
        // Move-only and unreachable from outside: the image's row in the ledger, opened by the constructor
        // above and closed by ~Image. An image cannot be alive without one or dead with one.
        ResourceOwnership m_Accounting;
    };

    class Image2D : public Image, public DynamicResources
    {
    public:
        Image2D() : Image( ResourceKind::Image2D )
        {
        }

        virtual ~Image2D() = default;

        virtual Core::Formats::Image2DSpecification& GetImageSpecification() = 0;

        /// Reads the image back to CPU as tightly-packed RGBA8 (size = width*height*4). Used for
        /// offscreen thumbnail capture (render -> readback -> PNG).
        ///
        /// AN EMPTY VECTOR USED TO BE THE ANSWER TO EVERY QUESTION HERE, and that is §1.4 of the
        /// contract exactly: this returned `{}` for an unsupported format, a failed staging allocation,
        /// a missing command buffer AND a failed readback, so "the capture did not happen" and "the
        /// image is empty" were one value. A thumbnail renderer cannot tell those apart, so it either
        /// caches a blank PNG forever or retries something that will never work — both happened.
        NO_DISCARD virtual Common::ResultStr<std::vector<uint8_t>> ReadPixelsRGBA8()
        {
            return Common::MakeError<std::vector<uint8_t>>(
                 "Image2D::ReadPixelsRGBA8 not supported by this backend" );
        }

        // Re-uploads tightly-packed pixel data into the EXISTING GPU image without recreating it — the
        // image (and any descriptor sets bound to its pointer) stays valid, so this is the safe, churn-free
        // way to stream changing content (video frames) every frame. `data` must match the image's format
        // and dimensions. Default: unsupported.
        NO_DISCARD virtual Common::BoolResultStr SetData( const Core::Formats::ImagePixelData& /*data*/ )
        {
            return Common::MakeError<bool>( "Image2D::SetData not supported by this backend" );
        }

        // NO MIP GENERATOR PARAMETER. Its last non-null argument was `Texture2D`'s source-file path, which
        // blitted a chain on the GPU after decoding a PNG/HDR. A 2D image's levels come from the cooked
        // container now (`Image2DSpecification::MipLevels`) or it has one level; see Texture.hpp.
        static std::shared_ptr<Image2D> Create( const Core::Formats::Image2DSpecification& spec );
    };

    class ImageCube : public Image, public DynamicResources
    {
    public:
        ImageCube() : Image( ResourceKind::ImageCube )
        {
        }

        virtual ~ImageCube() = default;

        virtual Core::Formats::ImageCubeSpecification& GetImageSpecification() = 0;

        static std::shared_ptr<ImageCube> Create( const Core::Formats::ImageCubeSpecification& spec,
                                                  const std::unique_ptr<MipMapCubeGenerator>&  mipGenerator );
        // `Copy()` was declared here and is gone — Image.cpp records what it did and why restoring it in
        // that shape would be a double free rather than a copy.
    };

    /**
     * @brief A volume texture: sampled as `sampler3D`, written by compute as `image3D`.
     *
     * Single mip level by design — see the note on Core::Formats::Image3DSpecification. Create() takes no
     * mip generator for the same reason: an argument that could only ever be ignored.
     *
     * The backend gives every volume a LINEAR / REPEAT sampler that does NOT follow the global Scene
     * Settings texture filter. Interpolating a noise volume is part of the algorithm, not a quality
     * preference: with "Nearest" selected for textures, a trilinearly-sampled noise volume would turn
     * into visible voxels.
     */
    class Image3D : public Image, public DynamicResources
    {
    public:
        Image3D() : Image( ResourceKind::Image3D )
        {
        }

        virtual ~Image3D() = default;

        // GetDepth() was here and had no caller: a volume's depth is read off the specification below,
        // beside its width and height, rather than asked of the image a second way. Г12.
        virtual Core::Formats::Image3DSpecification& GetImageSpecification() = 0;

        static std::shared_ptr<Image3D> Create( const Core::Formats::Image3DSpecification& spec );
    };

    namespace Utils
    {
        bool                   IsDepthFormat( Core::Formats::ImageFormat format );
        bool                   HasStencilComponent( Core::Formats::ImageFormat format );
        inline uint32_t        CalculateMipCount( uint32_t width, uint32_t height, uint32_t depth = 1 )
        {
            // Parenthesize to defeat any windows.h max() macro that may be active in the including TU.
            // Delegates to the ONE chain-length definition rather than rounding log2 its own way — a
            // second implementation here is exactly the "two implementations of one quantity" defect.
            return Core::Formats::MipChainLength( ( std::max )( { width, height, depth } ) );
        }
    } // namespace Utils

} // namespace Desert::Graphic

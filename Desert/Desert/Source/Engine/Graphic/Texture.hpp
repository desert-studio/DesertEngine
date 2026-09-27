#pragma once

#include <Engine/Graphic/Image.hpp>
#include <filesystem>

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Runtime/ImageHandle.hpp>

namespace Desert::Runtime
{
    class ImageService;
} // namespace Desert::Runtime

namespace Desert::Graphic
{
    class Texture
    {
    public:
        virtual ~Texture() = default;

        virtual uint32_t GetWidth() const  = 0;
        virtual uint32_t GetHeight() const = 0;

        virtual const Runtime::ImageHandle& GetImageHandle() const = 0;
    };

    class Texture2D final : public Texture
    {
    public:
        Texture2D() = default;
        // THE TEXTURE OWNS ITS IMAGE. Both factories register the image in the ImageService, and this is
        // where it comes back out: before, the destructor was the default and nothing unregistered, so
        // every asset texture, cooked panorama, GIF frame, video frame and BRDF LUT stayed resident until
        // the process exited, and the one owner that did release (Render2D's white texture) had to say it
        // by hand. The image's own destructor hands the VkImage to the allocator's deferred deletion
        // queue, so dropping a texture mid-frame is safe -- the same rule as OwnedEnvironment.
        ~Texture2D() override;

        // One owner of the registration: a copy would unregister the same handle twice.
        Texture2D( const Texture2D& )            = delete;
        Texture2D& operator=( const Texture2D& ) = delete;
        Texture2D( Texture2D&& )                 = delete;
        Texture2D& operator=( Texture2D&& )      = delete;

        static constexpr Core::Formats::Image2DUsage Type = Core::Formats::Image2DUsage::Image2D;

        virtual uint32_t GetWidth() const override
        {
            return m_Width;
        }
        virtual uint32_t GetHeight() const override
        {
            return m_Height;
        }

        virtual const Runtime::ImageHandle& GetImageHandle() const override
        {
            return m_Handle;
        }

        // THERE IS NO "FROM A SOURCE IMAGE FILE" FACTORY, and there must not be one. `Create(path)` used
        // to decode a PNG/HDR right here through stb, and its last caller was the sky panorama. A source
        // format is an import path and not a storage format (`Docs/Textures/T2_CONTAINER_DECISION.md`):
        // the editor's `TextureImporter` is the one place that decodes, and everything at run time reads
        // the container it writes. Desert/Tests/Engine/RuntimeSourceDecoders keeps it that way.

        /// Builds a texture from a texture ASSET (`.detex`): its platform data -- pixels and the whole mip
        /// chain -- comes out of the DDC under the key the asset describes (`LoadTexturePlatformData`) and
        /// is uploaded as it is. No image decoder runs here, and no blit chain is built.
        ///
        /// The refusal names the file and the reason — a stale JSON manifest, a truncated container, a
        /// level table that does not describe the file — because "the texture is missing" with no
        /// sentence attached is the most expensive kind of missing.
        static Common::ResultStr<std::shared_ptr<Texture2D>> CreateFromAsset( const std::filesystem::path& asset );

        // Creates the texture from CPU-generated pixel data (no file involved) — e.g. the runtime
        // BRDF LUT. `data` layout must match `format` (RGBA32F -> vector<float>, RGBA8F -> vector<uchar>).
        static Common::ResultStr<std::shared_ptr<Texture2D>> Create( const std::string& tag, uint32_t width,
                                                                     uint32_t                        height,
                                                                     Core::Formats::ImageFormat      format,
                                                                     Core::Formats::ImagePixelData&& data );

    private:
        // Registers `image` and records WHERE, so the destructor releases it from the same service.
        void AdoptImage( std::shared_ptr<Image2D>&& image );

        Runtime::ImageHandle m_Handle;
        // The service the image was registered in; null only while no image has been registered (a
        // factory that refused before the upload), which is the one case with nothing to release.
        // Not owned: the ImageService is a ResourceRegistry static constructed before any service that
        // can hold a texture, so it outlives them all (ResourceRegistry.cpp, "CONSTRUCTED FIRST").
        Runtime::ImageService* m_Service = nullptr;
        uint32_t               m_Width = 0, m_Height = 0;
    };

} // namespace Desert::Graphic

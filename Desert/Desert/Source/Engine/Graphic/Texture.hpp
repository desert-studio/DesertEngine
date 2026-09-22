#pragma once

#include <Engine/Graphic/Image.hpp>
#include <filesystem>

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Runtime/ImageHandle.hpp>

namespace Desert::Graphic
{
    struct TextureSpecification
    {
        bool GenerateMips = true;
    };

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
        Texture2D( const TextureSpecification& specification, const std::filesystem::path& path );
        virtual ~Texture2D() = default;

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

        /// Builds a texture from a SOURCE IMAGE FILE (PNG/HDR/...), decoding it here and generating the
        /// mip chain on the GPU. One caller is left: the sky panorama, whose `.hdr` is named directly by
        /// a SkyboxAsset and has never been cooked. Cooked textures do NOT come through here any more —
        /// see CreateFromCooked below, and `Docs/Textures/T2_CONTAINER_DECISION.md` for why a source
        /// format is an import path and not a storage format.
        static Common::ResultStr<std::shared_ptr<Texture2D>> Create( const TextureSpecification&  specification,
                                                                     const std::filesystem::path& path );

        /// Builds a texture from a cooked `.tex` container: its pixels and its whole mip chain are read
        /// out of the file and uploaded as they are. No image decoder runs, and no blit chain is built.
        ///
        /// The refusal names the file and the reason — a stale JSON manifest, a truncated container, a
        /// level table that does not describe the file — because "the texture is missing" with no
        /// sentence attached is the most expensive kind of missing.
        static Common::ResultStr<std::shared_ptr<Texture2D>>
        CreateFromCooked( const std::filesystem::path& cookedPath );

        // Creates the texture from CPU-generated pixel data (no file involved) — e.g. the runtime
        // BRDF LUT. `data` layout must match `format` (RGBA32F -> vector<float>, RGBA8F -> vector<uchar>).
        static Common::ResultStr<std::shared_ptr<Texture2D>>
        Create( const TextureSpecification& specification, const std::string& tag, uint32_t width,
                uint32_t height, Core::Formats::ImageFormat format, Core::Formats::ImagePixelData&& data );

    private:
        Common::BoolResultStr Invalidate();

    private:
        std::filesystem::path m_TexturePath;
        TextureSpecification  m_Specification;

        Runtime::ImageHandle m_Handle;
        uint32_t             m_Width = 0, m_Height = 0;
    };

    class TextureCube final : public Texture
    {
    public:
        TextureCube( const TextureSpecification& specification, const std::filesystem::path& path );
        virtual ~TextureCube() = default;

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

        static Common::ResultStr<std::shared_ptr<TextureCube>> Create( const TextureSpecification&  specification,
                                                                       const std::filesystem::path& path );

    private:
        Common::BoolResultStr Invalidate();

    private:
        std::filesystem::path m_TexturePath;
        TextureSpecification  m_Specification;

        Runtime::ImageHandle m_Handle;
        uint32_t             m_Width = 0, m_Height = 0;
    };

} // namespace Desert::Graphic

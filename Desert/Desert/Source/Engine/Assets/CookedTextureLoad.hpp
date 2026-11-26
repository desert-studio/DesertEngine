#pragma once

#include <Engine/Graphic/Texture.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Desert::Assets
{
    class TextureAsset;

    // THE COOKED TEXTURE'S CPU HALF (BINT-RC). Reading a texture asset is asset policy -- the DDC read and the
    // cook on a DDC miss (LoadTexturePlatformData), then the container decode (DecodeTextureBinary) -- so it lives
    // here in Engine, above Graphic::Texture2D (RenderCore), which only creates and uploads what this hands it
    // (Texture2D::CreateFromCooked).

    /// Builds a texture from a texture ASSET (`.detex`): its platform data -- pixels and the whole mip chain --
    /// comes out of the DDC under the key the asset describes (`LoadTexturePlatformData`) and is uploaded as it
    /// is. No image decoder runs here, and no blit chain is built.
    ///
    /// The refusal names the file and the reason -- a stale JSON manifest, a truncated container, a level table
    /// that does not describe the file -- because "the texture is missing" with no sentence attached is the most
    /// expensive kind of missing.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<Graphic::Texture2D>>
    CreateTextureFromAsset( const std::filesystem::path&   asset,
                            const Graphic::TextureBackend& backend = Graphic::TextureBackend::Device() );

    /// `CreateTextureFromAsset` in its two halves (AM2), so the on-demand path can put them on different threads.
    /// `ReadCookedTexture` is CPU only -- the DDC read, and the cook on a DDC miss, then the container decode --
    /// and is safe on a worker. `Texture2D::CreateFromCooked` creates the GPU image and uploads it, on the main
    /// thread.
    [[nodiscard]] Common::ResultStr<Graphic::CookedTexture2D>
    ReadCookedTexture( const std::filesystem::path& cookedPath );

    /// The streaming read (TEX-STREAMING): `ReadCookedTexture`, then only the levels from @p firstMip down are
    /// kept (CookedTexture2D::KeepFromMip). @p firstMip = Texture2D::kStreamingTail keeps the mip tail: the first
    /// level no larger than @p tailSize on its longer side, which is all a texture holds before the frame asks.
    [[nodiscard]] Common::ResultStr<Graphic::CookedTexture2D>
    ReadCookedTextureMips( const std::filesystem::path& cookedPath, uint32_t firstMip, uint32_t tailSize );

    /// PIXELS COME OUT OF THE COOKED `.tex` ITSELF, not out of the source image it was made from. This used to
    /// call `Texture2D::Create( {}, asset->GetSourcePath() )` -- the PNG -- so every texture in the project was
    /// entropy-decoded on the frame that first touched it and then had its mip chain blitted on the GPU. Neither
    /// survives the next step of the programme: a block-compressed format cannot be blit into (`blitDst=0`,
    /// re-measured by `Docs/Textures/bcprobe.c`), and "read only mip 4" is not expressible against a PNG at all.
    ///
    /// A failure is LOGGED with the path and the reason before nullptr comes back: this used to fail in silence,
    /// and a texture that quietly is not there is the most expensive kind of missing.
    [[nodiscard]] std::shared_ptr<Graphic::Texture2D>
    CreateTexture2D( const std::shared_ptr<TextureAsset>& asset );
} // namespace Desert::Assets

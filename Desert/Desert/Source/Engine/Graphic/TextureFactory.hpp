#pragma once

#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Graphic/Texture.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    class TextureFactory
    {
    public:
        // PIXELS COME OUT OF THE COOKED `.tex` ITSELF, not out of the source image it was made from.
        // This used to call `Texture2D::Create( {}, asset->GetSourcePath() )` — the PNG — so every
        // texture in the project was entropy-decoded on the frame that first touched it and then had
        // its mip chain blitted on the GPU. Neither survives the next step of the programme: a
        // block-compressed format cannot be blit into (`blitDst=0`, re-measured by
        // `Docs/Textures/bcprobe.c`), and "read only mip 4" is not expressible against a PNG at all.
        //
        // A failure is LOGGED with the path and the reason before nullptr comes back: this used to fail
        // in silence, and a texture that quietly is not there is the most expensive kind of missing.
        static std::shared_ptr<Texture2D> Create2D( const std::shared_ptr<Assets::TextureAsset>& asset )
        {
            if ( !asset )
            {
                LOG_ERROR( "[TextureFactory] Create2D was handed a null TextureAsset; no texture is built." );
                return nullptr;
            }

            const auto textureResult = Texture2D::CreateFromCooked( asset->GetMetadata().Filepath );
            if ( !textureResult.IsSuccess() )
            {
                LOG_ERROR( "[TextureFactory] Building the GPU texture for cooked asset '{0}' failed: {1}",
                           asset->GetMetadata().Filepath.string(), textureResult.GetError() );
                return nullptr;
            }

            return textureResult.GetValue();
        }
    };
} // namespace Desert::Graphic

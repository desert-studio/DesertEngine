#include "SourceAlphaMode.hpp"

#include <Editor/Import/DdsSource.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

#include <stb_image/stb_image.h>

#include <format>
#include <string_view>

namespace Desert::Editor
{
    namespace
    {
        // What the base colour image says about its alpha. `Read` false: the file could not be read or decoded
        // (no statement either way). `CutOut`: some texels fall below 0.5 and some reach it — a card's
        // silhouette, not a constant or empty alpha.
        struct AlphaContent
        {
            bool Read       = false;
            bool HasChannel = false;
            bool CutOut     = false;
        };

        // 0.5 in byte alpha: glTF's default cutoff and Falcor's alpha test, the cutoff a detected mask is given.
        constexpr unsigned char kCutOutThreshold = 128;

        bool CutsOut( const unsigned char* rgba, std::size_t texels )
        {
            bool below = false;
            bool above = false;
            for ( std::size_t t = 0; t < texels && !( below && above ); ++t )
            {
                const unsigned char a = rgba[t * 4u + 3u];
                below                 = below || a < kCutOutThreshold;
                above                 = above || a >= kCutOutThreshold;
            }
            return below && above;
        }

        // `file` is a loose image or the texture asset an embedded image was imported into (its carried source is
        // read). `scanTexels` false reads only the header (enough for "has no alpha"); true decodes the top mip —
        // a DDS through the cook's own decoder, anything else through stb_image — and reports `CutOut`.
        AlphaContent ReadAlphaContent( const std::filesystem::path& file, bool scanTexels )
        {
            AlphaContent out;
            if ( file.empty() )
                return out;
            const auto bytes = Assets::ReadTextureSourceImage( file );
            if ( !bytes.IsSuccess() )
                return out;
            // stb_image's C API and the DDS decoder take `const unsigned char*`, which may view any object's
            // bytes; the bytes reach them only through this cast.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto*       data = reinterpret_cast<const unsigned char*>( bytes.GetValue().data() );
            const std::size_t size = bytes.GetValue().size();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            if ( IsDdsSource( std::string_view( reinterpret_cast<const char*>( data ), size ),
                              file.generic_string() ) )
            {
                if ( !scanTexels )
                    return out; // a DDS header names a block format, not whether its alpha is used
                const auto image = DecodeDdsSource( data, size );
                if ( !image.IsSuccess() || image.GetValue().IsFloat )
                    return out;
                const DdsSourceImage& img = image.GetValue();
                out.Read                  = true;
                out.HasChannel            = true;
                out.CutOut = CutsOut( img.Rgba8.data(), static_cast<std::size_t>( img.Width ) * img.Height );
                return out;
            }
            int width    = 0;
            int height   = 0;
            int channels = 0;
            if ( stbi_info_from_memory( data, static_cast<int>( size ), &width, &height, &channels ) == 0 )
                return out;
            out.Read       = true;
            out.HasChannel = channels == 2 || channels == 4;
            if ( !scanTexels || !out.HasChannel )
                return out;
            stbi_uc* pixels =
                 stbi_load_from_memory( data, static_cast<int>( size ), &width, &height, &channels, 4 );
            if ( pixels == nullptr )
            {
                out.Read = false;
                return out;
            }
            out.CutOut = CutsOut( pixels, static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) );
            stbi_image_free( pixels );
            return out;
        }
    } // namespace

    SourceAlpha ResolveSourceAlpha( const aiMaterial& material, const std::filesystem::path& baseColourFile )
    {
        SourceAlpha out;

        aiString mode;
        if ( material.Get( AI_MATKEY_GLTF_ALPHAMODE, mode ) == AI_SUCCESS )
            out.AlphaMode = mode.C_Str();

        // A separate opacity map is the mask whatever else the file says (FBX cards, e.g. Poly Haven's
        // `*_alpha.png`): the lit passes prefer it over the albedo's alpha.
        if ( material.GetTextureCount( aiTextureType_OPACITY ) > 0 )
        {
            out.Kind        = SourceAlphaKind::OpacityMap;
            out.AlphaCutoff = 0.5f;
            return out;
        }

        const bool statesCutOut = out.AlphaMode == "MASK" || out.AlphaMode == "BLEND";
        if ( const AlphaContent header = statesCutOut ? ReadAlphaContent( baseColourFile, false ) : AlphaContent{};
             header.Read && !header.HasChannel )
        {
            out.Warning = std::format(
                 "[Import][Material] '{}' states alphaMode {} but base colour '{}' has no alpha "
                 "channel — imported opaque",
                 material.GetName().C_Str(), out.AlphaMode, baseColourFile.filename().generic_string() );
            return out; // Kind stays Opaque, AlphaCutoff 0
        }

        if ( out.AlphaMode == "MASK" )
        {
            float cutoff = kGltfDefaultAlphaCutoff;
            material.Get( AI_MATKEY_GLTF_ALPHACUTOFF, cutoff ); // absent -> glTF's default
            out.Kind        = SourceAlphaKind::Mask;
            out.AlphaCutoff = cutoff;
            return out;
        }

        if ( out.AlphaMode == "BLEND" )
        {
            out.Kind        = SourceAlphaKind::BlendAsMask;
            out.AlphaCutoff = kGltfDefaultAlphaCutoff;
            return out;
        }

        // NO statement at all (FBX, OBJ: only glTF names an alpha mode) and no opacity map: the cut-out, if any,
        // is the base colour's own alpha (UE's FBX import binds a diffuse texture's alpha to OpacityMask;
        // Falcor/ORCA's Bistro alpha-tests every material whose base colour carries one). Masked at 0.5 only when
        // the texels actually cut out — a constant-opaque alpha channel is no mask.
        if ( out.AlphaMode.empty() && ReadAlphaContent( baseColourFile, true ).CutOut )
        {
            out.Kind        = SourceAlphaKind::Mask;
            out.AlphaCutoff = kGltfDefaultAlphaCutoff;
            return out;
        }

        return out; // OPAQUE, or no statement and no cut-out alpha
    }
} // namespace Desert::Editor

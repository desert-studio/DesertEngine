#include "SourceAlphaMode.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

#include <stb_image/stb_image.h>

#include <format>

namespace Desert::Editor
{
    namespace
    {
        // True only when the image header was read and states no alpha channel (grey or RGB).
        bool ImageLacksAlpha( const std::filesystem::path& file )
        {
            int width    = 0;
            int height   = 0;
            int channels = 0;
            if ( file.empty() || stbi_info( file.string().c_str(), &width, &height, &channels ) == 0 )
                return false;
            return channels == 1 || channels == 3;
        }
    } // namespace

    SourceAlpha ResolveSourceAlpha( const aiMaterial& material, const std::filesystem::path& baseColourFile )
    {
        SourceAlpha out;

        aiString mode;
        if ( material.Get( AI_MATKEY_GLTF_ALPHAMODE, mode ) == AI_SUCCESS )
            out.AlphaMode = mode.C_Str();

        // A separate opacity map is the mask whatever else the file says (FBX cards, e.g. Poly Haven's
        // `*_alpha.png`): the PBR passes prefer it over the albedo's alpha.
        if ( material.GetTextureCount( aiTextureType_OPACITY ) > 0 )
        {
            out.Kind        = SourceAlphaKind::OpacityMap;
            out.AlphaCutoff = 0.5f;
            return out;
        }

        if ( ( out.AlphaMode == "MASK" || out.AlphaMode == "BLEND" ) && ImageLacksAlpha( baseColourFile ) )
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

        return out; // OPAQUE, or no statement at all
    }
} // namespace Desert::Editor

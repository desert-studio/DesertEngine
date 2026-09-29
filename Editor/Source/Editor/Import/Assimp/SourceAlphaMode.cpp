#include "SourceAlphaMode.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

namespace Desert::Editor
{
    SourceAlpha ResolveSourceAlpha( const aiMaterial& material )
    {
        SourceAlpha out;

        aiString mode;
        if ( material.Get( AI_MATKEY_GLTF_ALPHAMODE, mode ) == AI_SUCCESS )
            out.AlphaMode = mode.C_Str();

        // A separate opacity map is the mask whatever else the file says (FBX cards, e.g. Poly Haven's
        // `*_alpha.png`): PBRSurfaceParams::MaskTexture prefers it over the albedo's alpha.
        if ( material.GetTextureCount( aiTextureType_OPACITY ) > 0 )
        {
            out.Kind        = SourceAlphaKind::OpacityMap;
            out.AlphaCutoff = 0.5f;
            return out;
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

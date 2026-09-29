#pragma once

#include <filesystem>
#include <string>

struct aiMaterial;

namespace Desert::Editor
{
    // How a source material states its cut-out, read ONCE at import (UE Interchange's glTF rule): glTF names
    // it in the material (`alphaMode` + `alphaCutoff`, the mask being the base colour's alpha), FBX by a
    // separate opacity map. The renderer has one mask source per material (PBRSurfaceParams::MaskTexture) and
    // no translucent blend mode for surface materials, so every stated cut-out becomes a cutoff here.
    enum class SourceAlphaKind
    {
        Opaque,      // nothing stated: no cut-out
        OpacityMap,  // FBX: a separate opacity texture, read by its red channel
        Mask,        // glTF alphaMode MASK: the base colour's alpha against the file's own cutoff
        BlendAsMask, // glTF alphaMode BLEND: drawn as a mask at 0.5 (no translucent surface mode exists)
    };

    struct SourceAlpha
    {
        SourceAlphaKind Kind        = SourceAlphaKind::Opaque;
        float           AlphaCutoff = 0.0f; // 0 = no cut-out; what PBRSurfaceParams::AlphaCutoff receives
        std::string     AlphaMode;          // the glTF statement verbatim ("" when the file made none)
        std::string     Warning;            // non-empty: what the importer must LOG_WARN about this material
    };

    // glTF's default cutoff for `alphaMode: MASK` when the file states none (glTF 2.0 §5.19).
    inline constexpr float kGltfDefaultAlphaCutoff = 0.5f;

    // `baseColourFile` is the base colour texture as found on disk (empty when there is none). A glTF MASK or
    // BLEND takes its mask from that image's alpha; when the image has NO alpha channel (a JPG, an RGB PNG)
    // there is no mask to honour, so the material is imported OPAQUE and `Warning` names material and file.
    SourceAlpha ResolveSourceAlpha( const aiMaterial& material, const std::filesystem::path& baseColourFile = {} );
} // namespace Desert::Editor

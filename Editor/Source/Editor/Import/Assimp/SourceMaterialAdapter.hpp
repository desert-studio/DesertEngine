#pragma once

#include <Editor/Import/Assimp/SourceAlphaMode.hpp>
#include <Editor/Import/MaterialImportContract.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

struct aiMaterial;

namespace Desert::Editor
{
    // THE SOURCE ADAPTER (MAT1b): what assimp read from a file, spelled as the source format's own dictionary
    // (`gltf.baseColorTexture`, `gltf.KHR_materials_clearcoat`, `fbx.DiffuseColor`, ...). It never names a
    // template or a Property: the chosen template's Import rows do that (MaterialImportContract).
    //
    // glTF keys: baseColorFactor/Texture, metallicFactor, roughnessFactor, metallicRoughnessTexture,
    // occlusionTexture + occlusionStrength, normalTexture + normalScale, emissiveFactor/Texture +
    // emissiveStrength (KHR_materials_emissive_strength), alphaCutoff + alphaMask (the base colour image, value
    // 3 = its alpha channel) for alphaMode MASK/BLEND, uvOffset/uvScale/uvRotation (KHR_texture_transform of
    // the base colour), texCoord (a texture on a set other than 0), doubleSided, KHR_materials_unlit (presence),
    // and the extensions no shipped template reads (clearcoat, transmission, sheen, specular, ior, volume) so the
    // importer can say they were lost. FBX keys are preliminary (MAT1b-3): DiffuseColor, NormalMap,
    // EmissiveColor, TransparentColor, alphaCutoff.
    struct SourceMaterialRead
    {
        SourceMaterial Material;
        SourceAlpha    Alpha; // how the source stated its cut-out, for the importer's log
    };

    // "gltf" for .gltf/.glb, "fbx" otherwise (the formats assimp brings into the mesh importer).
    std::string_view SourceFormatOf( const std::filesystem::path& sourceFile );

    // `findTexture` maps a texture reference as the file spells it to the image on disk (empty = not found).
    SourceMaterialRead
    ReadSourceMaterial( const aiMaterial& material, std::string_view format, std::string name,
                        const std::function<std::filesystem::path( const std::string& )>& findTexture );
} // namespace Desert::Editor

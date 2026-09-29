#pragma once

#include <Common/Content/ShaderAssetHeader.hpp>

#include <glm/vec4.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Editor
{
    // MAT1 IMPORT ADAPTERS — the core's half. The importer carries a source material as THE SOURCE'S OWN
    // DICTIONARY (`gltf.baseColorTexture`, `gltf.KHR_materials_unlit`, `fbx.DiffuseColor`, ...) and never
    // names a template or a Property; each template's `Import` block (Common::Content::ShaderManifest) says
    // which keys feed which of its Properties. The core picks the template, then the template's rows fill the
    // .demat — so a new shading model is a new shader file, not an importer edit.

    // One key's payload. A key may carry both halves (FBX binds a map to a colour property); the Property's
    // kind decides which half a row takes. A presence key (`gltf.KHR_materials_unlit`) carries neither.
    struct SourceMaterialEntry
    {
        std::optional<glm::vec4>             Value;   // a factor or colour; a scalar sits in .x
        std::optional<std::filesystem::path> Texture; // the source image file the key binds
    };

    struct SourceMaterial
    {
        std::string                                             Name;
        std::map<std::string, SourceMaterialEntry, std::less<>> Entries;

        bool Has( std::string_view key ) const
        {
            return Entries.find( key ) != Entries.end();
        }
    };

    // A template as the importer sees it: the shader's declared name and its manifest.
    struct ImportTemplate
    {
        std::string                     ShaderName;
        Common::Content::ShaderManifest Manifest;
    };

    // THE CHOICE, and the only one. A template TAKES a source material when it declares an Import block, the
    // material carries every key the template `Requires`, and at least one of the template's rows reads a key
    // the material carries (a material with an empty dictionary is taken by any template that requires
    // nothing). Among the takers the one that requires MOST wins (Unlit's `gltf.KHR_materials_unlit` beats
    // StaticMeshPBR); a tie goes to the `Default Surface` template. No taker, or a tie nothing breaks, is a
    // refusal naming the material and the source file — never a fallback to a template by name.
    // Returns the index into `templates`.
    Common::ResultStr<std::size_t> ChooseImportTemplate( const SourceMaterial&           material,
                                                         std::span<const ImportTemplate> templates,
                                                         std::string_view                sourcePath );
} // namespace Desert::Editor

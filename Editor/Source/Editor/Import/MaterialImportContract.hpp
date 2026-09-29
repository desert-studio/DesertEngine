#pragma once

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Engine/Assets/MaterialData.hpp>

#include <glm/vec4.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
        // The Properties the shader declares as textures: a row into one of these takes its key's Texture
        // half, a row into any other Property its Value half ("the Property's kind decides").
        std::set<std::string, std::less<>> TextureProperties;
        std::string                        Guid;    // the shader's header GUID (the .demat's Shader reference)
        std::string                        Locator; // "engine:Shaders/..." — the reference's Path half
    };

    // A shader file as an import template: its declared name, manifest, texture Properties (the DSL parser's
    // own schema, so the kinds have one home) and header GUID. Refuses a file any of those readers refuse.
    Common::ResultStr<ImportTemplate> ReadImportTemplate( std::string_view source, std::string locator );

    // What a template's rows make of one source material.
    struct ImportedParam
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );
    };
    // One source image feeding a slot, and the channels it gives (empty = the whole image). A channel keeps
    // its place: `.gb` of the source becomes G and B of the slot.
    struct ImportedTexturePart
    {
        std::filesystem::path Source;
        std::string           Channels;
    };
    struct ImportedTextureSlot
    {
        std::string                      Slot;
        std::vector<ImportedTexturePart> Parts;
        // Every channel the template's rows route into this slot (empty = some row takes the whole image).
        std::string TemplateChannels;
        // The slot binds one source image AS IS only when one image fills every channel the template routes
        // here; otherwise the importer packs the parts into a derived image (a glTF occlusion map that is not
        // the metallic-roughness image, or a metallic-roughness image whose R is not occlusion at all).
        bool NeedsPacking() const;
    };
    struct TemplateFill
    {
        std::vector<ImportedParam>       Params;
        std::vector<ImportedTextureSlot> Textures;
        // Keys the source carries that no row of the chosen template reads: the importer warns with the
        // material and the key (a glTF clearcoat under a template without clearcoat is lost, and says so).
        std::vector<std::string> UnreadKeys;
    };
    TemplateFill FillFromTemplate( const SourceMaterial& material, const ImportTemplate& chosen );

    // THE DOCUMENT AN IMPORT WRITES, before its textures: the chosen template as the Shader and the fill's
    // Params - and NOTHING ELSE. In particular NO PreviewMesh (owner, THM1j/k): every imported material's
    // thumbnail is the ball, as in UE, masked ones included; PreviewMesh is only ever authored in the Material
    // Editor. The importer adds the texture references (they need the textures imported) and the header.
    Assets::MaterialData ImportedMaterialDocument( const ImportTemplate& chosen, const TemplateFill& fill );

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

#pragma once

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Engine/Core/Formats/SamplerState.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

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
        // How the source samples that image (glTF sampler wrapS/wrapT/magFilter; FBX mapping mode); absent when
        // it states the engine default (Repeat/Repeat/Linear), so a .demat only carries a state that differs.
        std::optional<::Desert::Core::Formats::SamplerState> Sampler;
    };

    struct SourceMaterial
    {
        std::string                                             Name;
        std::map<std::string, SourceMaterialEntry, std::less<>> Entries;

        [[nodiscard]] bool Has( std::string_view key ) const
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
        // The `Intent(...)` each texture Property declares (absent = it does not say): what the importer
        // writes into a texture asset it creates for that slot (UE: the sampler type sets TC_Normalmap).
        std::map<std::string, ::Desert::Core::Formats::TextureIntent, std::less<>> TextureIntents;
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
        // The first part's source sampler (MAT1s); written to the .demat slot as MaterialAssetRef::Sampler.
        std::optional<::Desert::Core::Formats::SamplerState> Sampler;
        // Every channel the template's rows route into this slot (empty = some row takes the whole image).
        std::string TemplateChannels;
        // How the slot's image encodes its values (TEX-SRGB), stated by the SOURCE format for the key that fed
        // it (SourceKeyColorSpace): sRGB when any part is a colour key, Linear otherwise. The importer gives a
        // texture asset it CREATES this space; an existing asset keeps the one it states (a user's edit stays).
        ::Desert::Core::Formats::TextureColorSpace ColorSpace = ::Desert::Core::Formats::TextureColorSpace::Linear;
        // The slot binds one source image AS IS only when one image fills every channel the template routes
        // here; otherwise the importer packs the parts into a derived image (a glTF occlusion map that is not
        // the metallic-roughness image, or a metallic-roughness image whose R is not occlusion at all).
        [[nodiscard]] bool NeedsPacking() const;
    };
    // The colour space the source format states for the image a key names. glTF 2.0 §3.9: baseColorTexture and
    // emissiveTexture are sRGB-encoded, every other texture (normal, metallic-roughness, occlusion, masks) is
    // linear; FBX's DiffuseColor and EmissiveColor maps are colour, its other maps data. One home for the list.
    [[nodiscard]] ::Desert::Core::Formats::TextureColorSpace SourceKeyColorSpace( std::string_view sourceKey );

    struct TemplateFill
    {
        std::vector<ImportedParam>       Params;
        std::vector<ImportedTextureSlot> Textures;
        // Keys the source carries that no row of the chosen template reads: the importer warns with the
        // material and the key (a glTF clearcoat under a template without clearcoat is lost, and says so).
        std::vector<std::string> UnreadKeys;
        // The source's `<format>.doubleSided`: a property of the MATERIAL (MaterialData::TwoSided), read under
        // every template, because it is a pipeline permutation and not a template parameter.
        bool TwoSided = false;
    };
    TemplateFill FillFromTemplate( const SourceMaterial& material, const ImportTemplate& chosen );

    // THE FBX SPECULAR MAP'S MEANING (SourceImportSettings::SpecularMap), applied to a source dictionary before
    // the template fill. The adapter carries the map under FBX's own name, `fbx.SpecularColor` (UE links it to
    // the Specular input); `OcclusionRoughnessMetallic` re-keys it to `fbx.OcclusionRoughnessMetallic`, which
    // a template routes channel by channel (StandardSurface: the ORM slot, R/G/B as they stand), and
    // `RoughnessMetallic` to `fbx.RoughnessMetallic` (StandardSurface: G and B only, so the import packs the
    // slot with an R of no occlusion, as for a glTF metallic-roughness image). A stated packed map is the whole
    // of what it holds, as UE wires a texture straight into the input: the re-keyed entry carries a unit value,
    // which a template routes to the factors the image's channels multiply.
    inline constexpr std::string_view kFbxSpecularMapKey             = "fbx.SpecularColor";
    inline constexpr std::string_view kFbxOcclusionRoughnessMetalKey = "fbx.OcclusionRoughnessMetallic";
    inline constexpr std::string_view kFbxRoughnessMetalKey          = "fbx.RoughnessMetallic";
    SourceMaterial WithFbxSpecularMap( SourceMaterial material, Assets::FbxSpecularMap meaning );

    // Why an unread key is lost, beyond "no Import row": the one key whose meaning is the user's to state (the
    // FBX Specular map) names the setting that states it. Empty for every other key.
    std::string_view UnreadKeyHint( std::string_view key );

    // THE DOCUMENT AN IMPORT WRITES, before its textures: the chosen template as the Shader and the fill's
    // Params - and NOTHING ELSE. In particular NO PreviewMesh (owner, THM1j/k): every imported material's
    // thumbnail is the ball, as in UE, masked ones included; PreviewMesh is only ever authored in the Material
    // Editor. The importer adds the texture references (they need the textures imported) and the header.
    Assets::MaterialData ImportedMaterialDocument( const ImportTemplate& chosen, const TemplateFill& fill );

    // THE CHOICE, and the only one. A template TAKES a source material when it declares an Import block, the
    // material carries every key the template `Requires`, and at least one of the template's rows reads a key
    // the material carries (a material with an empty dictionary is taken by any template that requires
    // nothing). Among the takers the one that requires MOST wins (Unlit's `gltf.KHR_materials_unlit` beats
    // StaticMeshLit); a tie goes to the `Default Surface` template. No taker, or a tie nothing breaks, is a
    // refusal naming the material and the source file — never a fallback to a template by name.
    // Returns the index into `templates`.
    Common::ResultStr<std::size_t> ChooseImportTemplate( const SourceMaterial&           material,
                                                         std::span<const ImportTemplate> templates,
                                                         std::string_view                sourcePath );
} // namespace Desert::Editor

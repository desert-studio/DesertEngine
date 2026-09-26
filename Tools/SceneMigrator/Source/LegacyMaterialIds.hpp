#pragma once

// THE OLD MATERIAL NUMBERS AND THE GUIDS THEY BECAME (AF7c).
//
// MATL v1 gave every `.demat` two identities: the header GUID and a u64 `MaterialId` in the payload, and
// everything that referenced a material - an instance's `ParentMaterialId`, a scene's `MaterialGuids`, a
// mesh slot - named it by the u64. MATL v2 keeps only the GUID (a material's handle is HandleForGuid of it),
// so each old number has to be translated to the GUID of the file that stated it.
//
// The translation cannot be recomputed once the materials are raised: a v2 file no longer states its old
// number, and the old numbers were partly random, so no rule derives them. The map is therefore built ONCE
// from the v1 corpus and kept as a register beside the assets root (LegacyMaterialIdRegisterPath). The
// material step (RaiseMaterialTextToV2) reads it for `ParentMaterialId`; the scene step that rewrites
// `MaterialGuids` (SCNE 27) reads the same register after the materials are already v2.

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Migration
{
    // Old MaterialId -> the GUID of the `.demat` that stated it.
    using LegacyMaterialIdMap = std::map<uint64_t, Common::Content::AssetGuid>;

    // The register lives NEXT TO the assets root, not in it: it is migration data, not content, and a file
    // inside the root would be a content file no asset kind claims.
    inline constexpr const char* kLegacyMaterialIdRegisterName = "LegacyMaterialIds.json";
    std::filesystem::path        LegacyMaterialIdRegisterPath( const std::filesystem::path& assetsRoot );

    // What one `.demat` text states about identity, read from the text without the material struct (whose
    // v2 shape no longer has the v1 members).
    struct StatedMaterialIds
    {
        uint32_t                   Version = 0; // the header's MATL version; 0 = no header
        Common::Content::AssetGuid Guid;        // null when there is no header
        std::optional<uint64_t>    MaterialId;
        std::optional<uint64_t>    ParentMaterialId;
    };
    Common::ResultStr<StatedMaterialIds> ReadStatedMaterialIds( std::string_view source, const std::string& text );

    // Records `id -> guid`; refuses when `id` already names a DIFFERENT GUID (two files, one old number - the
    // collision MaterialService refused at run time, now refused before any file is rewritten).
    Common::BoolResultStr AddLegacyMaterialId( LegacyMaterialIdMap& map, uint64_t id,
                                               const Common::Content::AssetGuid& guid, std::string_view source );

    // The register's text (canonical JSON, rows sorted by id) and its inverse.
    Common::ResultStr<std::string>         WriteLegacyMaterialIdRegister( const LegacyMaterialIdMap& map );
    Common::ResultStr<LegacyMaterialIdMap> ParseLegacyMaterialIdRegister( std::string_view   source,
                                                                          const std::string& text );

    // The register under `assetsRoot` (empty when there is none yet) merged with every `.demat` below
    // `assetsRoot` that still states a MaterialId. A file without a header takes the GUID the migration gives
    // it (MigrationGuidForPath of its path relative to `assetsRoot`), the same one the header step stamps.
    Common::ResultStr<LegacyMaterialIdMap> LoadLegacyMaterialIds( const std::filesystem::path& assetsRoot );

    // Writes the register under `assetsRoot` when its text would change; true when it was written.
    Common::ResultStr<bool> SaveLegacyMaterialIds( const std::filesystem::path& assetsRoot,
                                                   const LegacyMaterialIdMap&   map );

    // MATL 1 -> 2 on one `.demat` text, spliced so every other byte stays as written (the caller lays the
    // result out canonically): `MaterialId` removed, `ParentMaterialId` -> `Parent` naming the parent's GUID
    // through `map` and stated as the header's one Dependency, the header's MATL raised to 2. Refuses a text
    // that is not MATL v1, a parent the map does not know, and a header that already states dependencies.
    struct MaterialV2Report
    {
        bool DroppedId = false;
        bool Parented  = false;
    };
    Common::ResultStr<std::string> RaiseMaterialTextToV2( std::string_view source, const std::string& text,
                                                          const LegacyMaterialIdMap& map,
                                                          MaterialV2Report&          report );

    // MESH v1/v2 -> v3 (AF7q), here because the one thing the raise cannot do by layout alone is translate a
    // submesh's old material NUMBER, and that translation is this register. Byte-level on purpose: the engine's
    // decoder refuses a non-zero number by design, so decode + encode cannot carry one across.
    //
    // `meshGuid` becomes the v3 prefix GUID; every 128-byte submesh row gains the material's 16-byte GUID in
    // place of its 8-byte number (0 -> the null GUID, "no material"); a v1 table gains the empty PolyGroups
    // row; the sections are laid out again as the encoder lays them out. Refuses a foreign or truncated file,
    // a version other than 1 or 2 (a v3 file has nothing to raise - the pass skips it before calling), a
    // null `meshGuid`, and a non-zero number `map` does not know - by name, with no fallback.
    Common::ResultStr<std::string> UpgradeMeshBytesToV3( std::string_view source, std::string_view bytes,
                                                         const Common::Content::AssetGuid& meshGuid,
                                                         const LegacyMaterialIdMap&        map );

    // The cooked-mesh version `bytes` states, 1..kMeshBinaryVersion - the mesh pass's first question, asked
    // BEFORE it decides between "ok" and "raise". The signature is checked before the version is read: a
    // JSON-era mesh read as a header states "version" 1818322490 and was once reported "ok - already at v3".
    // Refuses by name: a file opening with `{` (a pre-binary JSON mesh, re-imported from source), any other
    // foreign or truncated file ("unknown mesh format"), the other byte order, and a version outside 1..current.
    Common::ResultStr<uint32_t> CookedMeshVersion( std::string_view source, std::string_view bytes );
    // ---- MATL 2 -> 3 (T6c3): a material's asset slots named by the referenced asset's header GUID ----------
    //
    // MATL 2 named every texture, cloud type, cloud layout and shader slot by a u64 derived from the asset's
    // PATH (AssetHandle::FromCookedPath: FNV over "<root tag>:<relative path>"). Those assets now register
    // under HandleForGuid of their header GUID, so the number reaches nothing; MATL 3 states the GUID itself
    // plus the path as a locator (Assets::MaterialAssetRef). Unlike the old MaterialId, the path-derived number
    // CAN be recomputed from the files that are still on disk, so no register is kept: the table is rebuilt
    // from the content root on every run.

    // THE MATL 2 SHAPE, FROZEN HERE and nowhere else: the engine's MaterialData is MATL 3 only, and every
    // pre-3 step (the v11 scene -> cloud material extraction, the O-4 layout split, the albedo broadcast)
    // reads and writes this. Members in the order the v2 writer stated them.
    struct MaterialParamV2
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );
    };
    struct MaterialTextureV2
    {
        std::string Name;
        uint64_t    TextureHandle = 0;
    };
    struct MaterialDataV2
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::optional<std::string>                                ShaderName;
        std::vector<MaterialParamV2>                              Params;
        std::vector<MaterialTextureV2>                            Textures;
        std::optional<std::string>                                Parent;
    };
    inline constexpr uint32_t kMaterialSchemaVersionV2 = 2;
    // The header versions a MATL 2 file states (the extraction step stamps them; the raise below replaces them).
    // Inline: every suite that compiles SceneMigration.cpp reaches it, most without this header's .cpp.
    inline std::span<const Common::Content::SubsystemVersion> MaterialTextSubsystemsV2()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kMaterialSchemaTag, kMaterialSchemaVersionV2 } };
        return versions;
    }

    // What an old number named: the asset's kind (which slot list may name it), its header GUID (empty for a
    // shader, which has none - Assets::MaterialShaderRef) and its locator (StableKeyForPath's spelling).
    enum class LegacyAssetKind
    {
        Texture,     // .detex (Texture and Skybox kinds)
        CloudType,   // .decloudtype
        CloudLayout, // .dclayout
        Shader,      // .shader under <assetsRoot>/../Shaders ("engine:" root)
    };
    struct LegacyAssetRef
    {
        LegacyAssetKind Kind = LegacyAssetKind::Texture;
        std::string     Guid;
        std::string     Path;
    };
    using LegacyAssetRefMap = std::map<uint64_t, LegacyAssetRef>;

    // Every `.detex`, `.decloudtype` and `.dclayout` under `assetsRoot` (key "assets:<relative>") and every
    // `.shader` under `<assetsRoot>/../Shaders` (key "engine:Shaders/<relative>"): FromKey(key) -> the file's
    // header GUID + key. Refuses an unreadable header and two files reaching one number, by name.
    Common::ResultStr<LegacyAssetRefMap> LoadLegacyAssetRefs( const std::filesystem::path& assetsRoot );

    // THE SHADER A MATL 2/3 `ShaderName` meant, as MATL 4 states it: the one `.shader` in `refs` whose file stem
    // is `shaderName` -> {its header GUID, its "engine:Shaders/..." key}. Refuses, naming `source` and the shader:
    // no such file, two files with that stem, and a file with no header GUID.
    Common::ResultStr<Assets::AssetGuidRef> ShaderRefByName( std::string_view source, std::string_view shaderName,
                                                             const LegacyAssetRefMap& refs );

    // One MATL 2 material raised to MATL 4: ShaderName -> Shader (ShaderRefByName), CloudType1..4 /
    // LayoutPattern / LayoutMask / Medium -> CloudAssets, every other slot -> Textures; a number becomes
    // {GUID, locator} through `refs`, a 0 an authored empty slot. The header is left for StampMaterialHeader
    // (WriteMaterialJson) to raise and fill with Dependencies. Refuses, naming `source`, the slot and the number:
    // a number `refs` does not know, a number naming an asset of another kind than the slot takes, an asset
    // with no header GUID, and a material with no header.
    Common::ResultStr<Assets::MaterialData>
    RaiseMaterialV2ToV4( std::string_view source, const MaterialDataV2& material, const LegacyAssetRefMap& refs );
} // namespace Desert::Migration

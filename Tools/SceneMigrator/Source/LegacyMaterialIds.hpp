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

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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
} // namespace Desert::Migration

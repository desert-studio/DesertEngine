#pragma once
// The body of an import record, `<name>.<ext>.deimport` (FIX8; UE: a .uasset's persistent GUID and its
// UAssetImportData). Path rules and the reason the record exists: Common/Content/ImportRecord.hpp.
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    /**
     * @brief The FILE layout's generation, stated under `DIMP`.
     *
     *   1 - the text asset header (Kind "StaticMesh", the GUID that IS the imported mesh's identity and, through
     *       HandleForGuid, its handle) and `Source`, the file name of the source the record belongs to (FIX8).
     *       No import settings yet: the importer has none that a user sets per source.
     *   2 - `Bounds`, the imported mesh's box in centimetres (Min/Max), written by every import and re-import
     *       (DIMP2). The registry reads it without loading anything - UE's asset registry serves a package's
     *       bounds tag the same way - so a cold DDC still knows the box. Required; a version-1 record is
     *       refused by its path and re-imported.
     *
     * An unknown value is refused in both directions.
     */
    inline constexpr int32_t kImportRecordVersion = static_cast<int32_t>( Assets::kImportRecordSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> ImportRecordTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kImportRecordSchemaTag,
                                                static_cast<uint32_t>( kImportRecordVersion ) } };
        return versions;
    }

    struct ImportRecordData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        /// The source's file name ("base.fbx"): a record copied beside another source is refused by name.
        std::string Source;
        /// The imported mesh's box (version 2): the same `Bounds` member a prefab states.
        struct Box
        {
            std::array<float, 3> Min{};
            std::array<float, 3> Max{};
        };
        std::optional<Box> Bounds;

        // THE IMPORT OPTION "Combine Meshes" (UE: UFbxStaticMeshImportData::bCombineMeshes, OFF by default).
        // Absent means UE's default, false: every mesh-bearing node of the source becomes its own static mesh
        // (NodeMeshSplit). Set it true in the record to import the whole file as the one combined mesh only.
        // Kept by every re-import (EnsureImportRecord rewrites the parsed record, never a fresh one).
        std::optional<bool> CombineMeshes;

        // THE NODE MESHES THE LAST SPLIT IMPORT WROTE (THM1j), by node name: <stem>_<node>.stmesh beside the
        // source (NodeMeshSplit). Present only when the source was split; then there is NO combined mesh, and
        // the import's freshness is these files' (each states the source's hash), not a combined envelope's.
        std::optional<std::vector<std::string>> Nodes;

        // HOW EACH MESH THIS IMPORT WRITES IS PHOTOGRAPHED (UE: UStaticMesh::ThumbnailInfo, a USceneThumbnailInfo
        // saved in the mesh's package). The record IS the imported mesh's package: the combined mesh lives in the
        // DDC and every node `.stmesh` is rewritten by each re-import, so an orbit stored in either would be lost;
        // the record is kept by every re-import (EnsureImportRecord rewrites the parsed record). Keyed by the
        // mesh asset's file name beside the source: the source's own name ("base.fbx") for the combined mesh,
        // `<stem>_<node>.stmesh` for a node mesh (NodeMeshAssetPath). A mesh without an entry has the default
        // orbit (ThumbnailInfo.hpp); a stated default is refused, so one picture has one spelling.
        std::optional<std::map<std::string, ThumbnailOrbit>> Thumbnail;
    };

    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text );
    std::string                         WriteImportRecord( const ImportRecordData& data );

    /// The GUID @p source's record states. An error naming the record's path when it is missing, unreadable,
    /// of another generation or written for another source - never a GUID made up from the path.
    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source );

    /// The import's side: the record's GUID, the record written first (with a new GUID) when @p source has
    /// none. An existing record keeps its GUID, so a re-import keeps the identity; its `Bounds` are rewritten
    /// when the import's box differs from the one it states.
    /// @p source's "Combine Meshes" option: the record's value, UE's default (false) when the record states none
    /// or when the source has no record yet (its first import). An error naming the record when it is unreadable.
    Common::ResultStr<bool> ReadImportRecordCombineMeshes( const std::filesystem::path& source );

    /// @p source's whole record; nullopt when the source has no record yet. An error naming the record when it is
    /// unreadable.
    Common::ResultStr<std::optional<ImportRecordData>> ReadImportRecord( const std::filesystem::path& source );

    /// Rewrites the record's `Nodes` (THM1j): the node names a split import wrote, nullopt for a combined import.
    /// The record must exist (EnsureImportRecord runs first); written only when the list changes.
    Common::BoolResultStr SetImportRecordNodes( const std::filesystem::path&                   source,
                                                const std::optional<std::vector<std::string>>& nodes );

    /// The orbit @p source's record states for the mesh asset named @p meshFile (ImportRecordData::Thumbnail);
    /// the default orbit when it states none for it. An error naming the record when it is missing or unreadable.
    Common::ResultStr<ThumbnailOrbit> ReadImportRecordThumbnail( const std::filesystem::path& source,
                                                                 const std::string&           meshFile );

    /// Rewrites the orbit @p source's record states for the mesh asset named @p meshFile (UE: Edit Thumbnail
    /// writes the asset's ThumbnailInfo). The default orbit removes the entry (the default is written as no key),
    /// the last removal the whole `Thumbnail` key. The record must exist; written only when the orbit changes. An
    /// error for an orbit IsValidThumbnailOrbit refuses.
    Common::BoolResultStr SetImportRecordThumbnail( const std::filesystem::path& source,
                                                    const std::string& meshFile, const ThumbnailOrbit& orbit );

    Common::ResultStr<Common::Content::AssetGuid> EnsureImportRecord( const std::filesystem::path& source,
                                                                      const Common::Math::AABB&    bounds );
} // namespace Desert::Assets::Serialization

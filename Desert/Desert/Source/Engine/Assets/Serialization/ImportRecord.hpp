#pragma once
// The body of an import record, `<name>.<ext>.deimport` (FIX8; UE: a .uasset's persistent GUID and its
// UAssetImportData). Path rules and the reason the record exists: Common/Content/ImportRecord.hpp.
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    /**
     * @brief The FILE layout's generation, stated under `DIMP`.
     *
     *   1 - the text asset header (Kind "StaticMesh" - or, since THM1l, what the source imports as: see
     *       IsImportRecordKind - the GUID that IS the imported mesh's identity and, through
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

    /// Assets::SourceImportSettings as text states it: the enums by NAME (MeshSourceUpAxisName /
    /// MeshLodPolicyName, the names IMPT uses), so reordering an enum never changes what a record says. The
    /// record's `Settings` and the editor's remembered Import Options are this shape.
    struct SourceImportSettingsText
    {
        bool        CombineMeshes = false;
        float       UniformScale  = 1.0f;
        std::string UpAxis;
        std::string LodPolicy;
    };
    [[nodiscard]] SourceImportSettingsText ImportSettingsToText( const Assets::SourceImportSettings& settings );
    /// Refused, by name, for an unknown up axis or LOD policy or a scale that is not finite and > 0.
    [[nodiscard]] Common::ResultStr<Assets::SourceImportSettings>
    ImportSettingsFromText( const SourceImportSettingsText& text );

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

        // THE SOURCE'S IMPORT OPTIONS (THM1l; UE UFbxImportUI): Combine Meshes, Uniform Scale, Up Axis, LOD
        // policy - the one home of what the Import Options window and the Details' Import Settings edit. Absent
        // means UE's defaults (SourceImportSettings{}). Written by every import with the options it ran with.
        std::optional<SourceImportSettingsText> Settings;

        // THE NODE MESHES THE LAST SPLIT IMPORT WROTE (THM1j), by node name: <stem>_<node>.stmesh beside the
        // source (NodeMeshSplit). Present only when the source was split; then there is NO combined mesh, and
        // the import's freshness is these files' (each states the source's hash), not a combined envelope's.
        std::optional<std::vector<std::string>> Nodes;
    };

    // THE RECORD STATES WHAT THE SOURCE IMPORTS AS (UE: an imported asset's class - a UStaticMesh, a
    // USkeletalMesh, a USkeleton or a UAnimSequence - is part of its identity): the header's Kind is StaticMesh
    // for a static file, SkinnedMesh for a skinned one, Skeleton for a skeleton with clips and no mesh, Animation
    // for clips only. Only a StaticMesh record stands for a mesh asset with no file of its own (ContentScan).
    [[nodiscard]] bool IsImportRecordKind( Common::Content::ContentKind kind );

    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text );
    /// Refused for a @p kind that is not IsImportRecordKind.
    Common::ResultStr<std::string> WriteImportRecord( const ImportRecordData&      data,
                                                      Common::Content::ContentKind kind );

    /// The GUID @p source's record states. An error naming the record's path when it is missing, unreadable,
    /// of another generation or written for another source - never a GUID made up from the path.
    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source );

    /// The import's side: the record's GUID, the record written first (with a new GUID) when @p source has
    /// none. An existing record keeps its GUID, so a re-import keeps the identity; its `Bounds` are rewritten
    /// when the import's box differs from the one it states.
    /// @p source's import options: the record's, UE's defaults when the record states none or when the source has
    /// no record yet (its first import). An error naming the record when it is unreadable.
    Common::ResultStr<Assets::SourceImportSettings>
    ReadImportRecordSettings( const std::filesystem::path& source );

    /// @p source's whole record; nullopt when the source has no record yet. An error naming the record when it is
    /// unreadable.
    Common::ResultStr<std::optional<ImportRecordData>> ReadImportRecord( const std::filesystem::path& source );

    /// Rewrites the record's `Nodes` (THM1j): the node names a split import wrote, nullopt for a combined import.
    /// The record must exist (EnsureImportRecord runs first); written only when the list changes.
    Common::BoolResultStr SetImportRecordNodes( const std::filesystem::path&                   source,
                                                const std::optional<std::vector<std::string>>& nodes );

    /// ... and its `Settings` rewritten to @p settings, the options this import ran with, and its header's Kind to
    /// @p kind, what the source imports as (IsImportRecordKind). Written by EVERY import, static or skinned (UE:
    /// AssetImportData on every imported asset): a source with a record is not new. @p bounds is the imported
    /// mesh's box in the ENGINE's space, the options applied (SourceToEngineBounds): the box the placed mesh has.
    /// A file with no mesh (skeleton and clips only) passes no @p bounds, and the record keeps the one it states.
    Common::ResultStr<Common::Content::AssetGuid>
    EnsureImportRecord( const std::filesystem::path& source, Common::Content::ContentKind kind,
                        const std::optional<Common::Math::AABB>& bounds,
                        const Assets::SourceImportSettings&      settings );
} // namespace Desert::Assets::Serialization

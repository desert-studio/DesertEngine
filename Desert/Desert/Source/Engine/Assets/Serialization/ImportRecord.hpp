#pragma once
// The body of an import record, `<name>.<ext>.deimport` (FIX8; UE: a .uasset's persistent GUID and its
// UAssetImportData). Path rules and the reason the record exists: Common/Content/ImportRecord.hpp.
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ImportRecord.hpp>
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
     *   1 - the text asset header (Kind "StaticMesh", the GUID that IS the imported mesh's identity and, through
     *       HandleForGuid, its handle) and `Source`, the file name of the source the record belongs to (FIX8).
     *       No import settings yet: the importer has none that a user sets per source.
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
    };

    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text );
    std::string                         WriteImportRecord( const ImportRecordData& data );

    /// The GUID @p source's record states. An error naming the record's path when it is missing, unreadable,
    /// of another generation or written for another source - never a GUID made up from the path.
    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source );

    /// The import's side: the record's GUID, the record written first (with a new GUID) when @p source has
    /// none. An existing record is never rewritten, so a re-import keeps the identity.
    Common::ResultStr<Common::Content::AssetGuid> EnsureImportRecord( const std::filesystem::path& source );
} // namespace Desert::Assets::Serialization

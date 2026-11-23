#pragma once

#include <Engine/Assets/DataAssetTypeRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    // A `.dasset` (ASSET-TYPES-OPEN; UE UPrimaryDataAsset): a game's typed data - a weapon, an ability, a loot
    // table - as ONE asset kind whose payload is a reflected struct named in the file:
    //   { "Header": {...}, "Type": "WeaponDef", "Data": { "Damage": 35.0, "FireRate": 600.0 } }
    // `Type` is the payload's reflected name (DataAssetTypeRegistry), `Data` its fields as
    // Reflection::SerializeReflected writes them. A game module registers the struct; the engine's AssetTypeID,
    // its switches and its loaders never learn the game's types.
    inline constexpr const char* kDataAssetExtension = ".dasset";

    inline constexpr int32_t kDataAssetVersion = static_cast<int32_t>( Assets::kDataAssetSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> DataAssetTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kDataAssetSchemaTag,
                                                static_cast<uint32_t>( kDataAssetVersion ) } };
        return versions;
    }

    /// The file as JSON states it; `Data` stays an untyped object until the registry names its struct.
    struct DataAssetFileData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               Type;
        Common::Json::Object                                      Data;
    };

    /// A parsed `.dasset`: the header, the registered type and a payload of that type.
    struct DataAssetContents
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        const DataAssetType*                                      Type = nullptr;
        std::shared_ptr<void>                                     Payload;
    };

    /// Parses a `.dasset`. Refused: no header, another kind or version, a `Type` no loaded module registered, or
    /// a `Data` member of the wrong type for its field (every such field named). A field the file does not state
    /// keeps the struct's default (a field added to the struct after the file was written).
    Common::ResultStr<DataAssetContents> ParseDataAsset( const std::string& text );

    /// The text of a `.dasset` holding @p payload (an object of @p type), its header stamped from @p header.
    Common::ResultStr<std::string>
    WriteDataAsset( const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                    const DataAssetType& type, const void* payload );

    Common::BoolResultStr
    SaveDataAssetFile( const std::filesystem::path&                                     path,
                       const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                       const DataAssetType& type, const void* payload );
} // namespace Desert::Assets::Serialization

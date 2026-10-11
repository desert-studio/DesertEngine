#pragma once

// VFX-10. The `.dfxch` file: one VFX DATA CHANNEL - the payload layout gameplay writes entries of and emitters
// spawn from (UE UNiagaraDataChannel: an asset holding the channel's variables, NiagaraDataChannel.h). The channel
// is named by its file stem, as a scene refers to it ("DataChannel.<stem>" in a module binding, VFXSystem.hpp).
// Our field types are the engine's own, not UE's free FNiagaraVariable list: a payload is what a spawn can bind or
// a filter can test, so every type has a reader (VFX/VFXDataChannel.hpp).

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kVFXDataChannelExtension = ".dfxch";

    /**
     * @brief The FILE layout's generation, stated in the text asset header under `VFXD` from the first file.
     *
     *   1 - VFX-10: Fields (Name, Type).
     */
    inline constexpr int32_t kVFXDataChannelVersion = static_cast<int32_t>( Assets::kVFXDataChannelSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> VFXDataChannelTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kVFXDataChannelSchemaTag,
                                                static_cast<uint32_t>( kVFXDataChannelVersion ) } };
        return versions;
    }

    /// The type of one payload field. Floats per entry: Position 3 (world centimetres), Direction 3 (unit length
    /// is the writer's business; a spawn uses it as the start velocity direction), Color 4 (linear rgba), Float 1,
    /// Int 1 (stored exactly: |value| < 2^24).
    enum class VFXDataChannelFieldType
    {
        Position,
        Direction,
        Color,
        Float,
        Int,
    };

    [[nodiscard]] uint32_t FloatCount( VFXDataChannelFieldType type );

    struct VFXDataChannelField
    {
        std::string             Name;
        VFXDataChannelFieldType Type = VFXDataChannelFieldType::Float;

        [[nodiscard]] bool operator==( const VFXDataChannelField& ) const = default;
    };

    /// The most fields one channel carries (an entry is at most 16 x 4 floats).
    inline constexpr std::size_t kVFXDataChannelMaxFields = 16;

    struct VFXDataChannelData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<VFXDataChannelField> Fields;

        [[nodiscard]] bool operator==( const VFXDataChannelData& ) const = default;
    };

    /// Rejects a channel with no field, more than kVFXDataChannelMaxFields, or a field whose name is empty,
    /// repeated or not an identifier ([A-Za-z_][A-Za-z0-9_]*), naming the field.
    Common::BoolResultStr ValidateVFXDataChannelData( const VFXDataChannelData& data );

    /// Parses a `.dfxch`. A file without a header, of another version or kind, or that states Dependencies is
    /// refused; so is anything ValidateVFXDataChannelData refuses.
    Common::ResultStr<VFXDataChannelData> ParseVFXDataChannel( const std::string& text );

    /// The canonical text of @p data with its header stamped (kind, VFXD version, no Dependencies).
    std::string WriteVFXDataChannel( const VFXDataChannelData& data );

    Common::BoolResultStr                 SaveVFXDataChannelFile( const std::filesystem::path& path,
                                                                  const VFXDataChannelData&    data );
    Common::ResultStr<VFXDataChannelData> LoadVFXDataChannelFile( const std::filesystem::path& path );
} // namespace Desert::Assets::Serialization

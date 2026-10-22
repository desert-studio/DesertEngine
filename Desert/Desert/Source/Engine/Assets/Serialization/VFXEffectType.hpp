#pragma once

// VFX-11. The `.dfxtype` file: one VFX EFFECT TYPE - the scalability a family of systems shares (UE
// UNiagaraEffectType, NiagaraEffectType.h: per-quality-level FNiagaraSystemScalabilitySettings and
// FNiagaraEmitterScalabilitySettings). A system names its type in its `.dfx` (VFXSystemData::EffectType, the
// type's file stem, as a module names a data channel); the world decides per type and per view level
// (VFXWorld::Tick). A project's systems all have one: the migrator gives every older system the project's
// `Default` type (UE UNiagaraSettings::DefaultEffectType).
//
// What a level states is only what VFXWorld honours: MaxDistance with UE's DeactivateResume reaction (no births
// past it, live particles finish, spawning resumes in range) and the emitter SpawnCountScale. MaxInstances,
// significance and frustum culling join with the significance step (VFX-11 plan step 3).

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
    inline constexpr const char* kVFXEffectTypeExtension = ".dfxtype";

    /**
     * @brief The FILE layout's generation, stated in the text asset header under `VFXT` from the first file.
     *
     *   1 - VFX-11: Levels (MaxDistance, SpawnCountScale), one per Effects quality level.
     */
    inline constexpr int32_t kVFXEffectTypeVersion = static_cast<int32_t>( Assets::kVFXEffectTypeSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> VFXEffectTypeTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kVFXEffectTypeSchemaTag,
                                                static_cast<uint32_t>( kVFXEffectTypeVersion ) } };
        return versions;
    }

    /// One Effects quality level's settings (UE FNiagaraSystemScalabilitySettings::MaxDistance with
    /// CullReaction DeactivateResume; FNiagaraEmitterScalabilitySettings::SpawnCountScale).
    struct VFXEffectTypeLevel
    {
        float MaxDistance     = 0.0f; ///< centimetres from the nearest view past which nothing spawns; 0 = never
        float SpawnCountScale = 1.0f; ///< births x this (with the Effects.SpawnCountScalePercent row); 0..1

        [[nodiscard]] bool operator==( const VFXEffectTypeLevel& ) const = default;
    };

    /// The number of levels a type states: one per Effects level, Low .. Cinematic (the Effects.QualityLevel row
    /// of Scalability.json indexes them, UE fx.Niagara.QualityLevel).
    inline constexpr std::size_t kVFXEffectTypeLevelCount = 5;

    struct VFXEffectTypeData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<VFXEffectTypeLevel> Levels;

        [[nodiscard]] bool operator==( const VFXEffectTypeData& ) const = default;
    };

    /// Rejects a type without exactly kVFXEffectTypeLevelCount levels, a negative MaxDistance or a
    /// SpawnCountScale outside 0..1, naming the level.
    Common::BoolResultStr ValidateVFXEffectTypeData( const VFXEffectTypeData& data );

    /// Parses a `.dfxtype`. A file without a header, of another version or kind, that states Dependencies or
    /// that ValidateVFXEffectTypeData refuses is an error naming why.
    Common::ResultStr<VFXEffectTypeData> ParseVFXEffectType( const std::string& text );

    /// The canonical text of @p data with its header stamped (kind, VFXT version, no Dependencies).
    std::string WriteVFXEffectType( const VFXEffectTypeData& data );

    Common::BoolResultStr SaveVFXEffectTypeFile( const std::filesystem::path& path,
                                                 const VFXEffectTypeData&     data );
} // namespace Desert::Assets::Serialization

#pragma once

#include <Engine/Assets/AssetBase.hpp>

#include <Audio/SoundProperties.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    inline constexpr std::string_view kSoundExtension = ".desound";
    inline constexpr std::string_view kSoundKind      = "Sound";

    /**
     * @brief A SOUND (`.desound`) — UE's USoundWave: the identity every sound reference names.
     *
     * THE FILE: a text asset header (Kind "Sound", the GUID, SNDW version), `Source` — the imported audio file's
     * name RELATIVE TO THE `.desound` (UE keeps the imported source beside the asset the same way) — `Class`,
     * the SoundClass every voice of it plays in (UE USoundBase::SoundClassObject), and `Loading`, how its PCM
     * reaches the mix (Resident or Streamed; UE ESoundWaveLoadingBehavior / bStreaming). A track or a component
     * names the GUID; path, class and loading live here and nowhere else, so moving the pair moves no reference
     * and every player of the sound plays it in the same class.
     * The bytes are not read at load: AudioEngine opens the source on its resource manager's job thread when a
     * voice starts (decoded once and shared when Resident, page by page when Streamed).
     *
     * SNDW v1 (AUD-CORE) added Class and Loading; a v0 file (no SNDW version) is refused by name and raised by
     * Tools/SceneMigrator (Upgrade) to Class Effects + Loading Resident — what every sound played as before.
     */
    class SoundAsset final : public AssetBase
    {
    public:
        SoundAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        /// The audio file, resolved against the `.desound`'s directory.
        [[nodiscard]] const std::filesystem::path& SourceFile() const
        {
            return m_SourceFile;
        }

        [[nodiscard]] Audio::SoundClass Class() const
        {
            return m_Class;
        }

        [[nodiscard]] Audio::SoundLoading Loading() const
        {
            return m_Loading;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Sound;
        }

        struct Parsed
        {
            Common::Content::AssetGuid Guid;
            std::string                Source; ///< as written: relative to the `.desound`
            Audio::SoundClass          Class;
            Audio::SoundLoading        Loading;
        };

        /// Refuses a header of another kind or SNDW version, a null/malformed GUID, an empty Source and a Class or
        /// Loading that names no enumerator, by name. A v0 file is refused with the migrator's name.
        [[nodiscard]] static Common::ResultStr<Parsed> Parse( std::string_view text );

        /// THE SNDW MIGRATION (Tools/SceneMigrator): the current text of an older `.desound`, nullopt when @p text
        /// is current already. v0 -> v1 states Class Effects and Loading Resident, what every voice played as
        /// before the asset carried them. Refuses a document that is not a `.desound` and a version this build
        /// does not know, by name.
        [[nodiscard]] static Common::ResultStr<std::optional<std::string>> Upgrade( std::string_view text );

        /// Reads and parses the `.desound` at @p desound. Refuses an unreadable file and every Parse refusal.
        [[nodiscard]] static Common::ResultStr<Parsed> ReadFile( const std::filesystem::path& desound );

        /// The `.desound` text naming @p source under @p guid, played in @p soundClass with @p loading. Refuses a
        /// null GUID and an empty source.
        [[nodiscard]] static Common::ResultStr<std::string> Write( const Common::Content::AssetGuid& guid,
                                                                   std::string_view                  source,
                                                                   Audio::SoundClass                 soundClass,
                                                                   Audio::SoundLoading               loading );

        /// What a voice of the sound @p guid plays (Audio::SoundSource): the registry row of the GUID (through
        /// redirectors), its `.desound` read and parsed, its Source resolved beside it, its Class and Loading.
        /// Refuses an unknown GUID and every Parse refusal, by name. What a sound REFERENCE resolves through —
        /// the path is never stored by a user.
        [[nodiscard]] static Common::ResultStr<Audio::SoundSource>
        Resolve( const Common::Content::AssetGuid& guid );

        /// The same, for a reference held as the GUID's handle (`AudioSourceData::Sound`).
        [[nodiscard]] static Common::ResultStr<Audio::SoundSource> Resolve( AssetHandle handle );

        /// Writes a `.desound` beside its source (creating the directory). Static: saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&           filepath,
                                           const Common::Content::AssetGuid& guid, std::string_view source,
                                           Audio::SoundClass soundClass, Audio::SoundLoading loading );

    private:
        Common::Content::AssetGuid m_Guid;
        std::filesystem::path      m_SourceFile;
        Audio::SoundClass          m_Class   = Audio::SoundClass::Effects; // overwritten by every LoadFromFile
        Audio::SoundLoading        m_Loading = Audio::SoundLoading::Resident;
        bool                       m_Ready   = false;
    };
} // namespace Desert::Assets

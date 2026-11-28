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
     * THE FILE: header (Kind "Sound", GUID, SNDW version, Dependencies = the class GUID), `Source` — the audio
     * file relative to the `.desound` — `Class`, the GUID text of the sound class asset every voice of it plays
     * in (UE USoundBase::SoundClassObject), and `Loading`.
     *
     * SNDW v2 (AUDIO-SOUNDCLASS): Class is a class asset's GUID. v1 wrote an enumerator name, v0 nothing; both
     * are refused by name and raised by Tools/SceneMigrator through Upgrade.
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

        [[nodiscard]] const Audio::SoundClassId& Class() const
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
            Audio::SoundClassId        Class;
            Audio::SoundLoading        Loading;
        };

        /// Refuses a header of another kind or SNDW version, a null/malformed GUID, an empty Source, a Class that
        /// is not a non-null GUID, Dependencies that are not exactly that GUID, and a Loading that names no
        /// enumerator, by name. An older file is refused with the migrator's name. Text only: whether the class
        /// asset exists is Resolve's and the voice's question.
        [[nodiscard]] static Common::ResultStr<Parsed> Parse( std::string_view text );

        /// What SNDW v1's Class names and v0's silence become: the class asset of each. The migrator fills it
        /// from the project (the class asset whose file stem is the v1 name); the format names no content.
        struct LegacyClasses
        {
            /// The GUID for a v1 `Class` name, or the null GUID when the project has no such class asset.
            Audio::SoundClassId ( *ForName )( std::string_view v1Name, const void* user ) = nullptr;
            const void*         User                                                      = nullptr;
            /// The name a v0 file (no Class at all) is raised under: what v0 -> v1 stated.
            std::string_view    V0Name;
        };

        /// THE SNDW MIGRATION (Tools/SceneMigrator): the current text of an older `.desound`, nullopt when @p text
        /// is current already. v0 -> v1 -> v2 in one call, each step naming its two versions. Refuses a document
        /// that is not a `.desound`, a version this build does not know, and a v1 Class name for which
        /// @p classes answers the null GUID (naming the Class).
        [[nodiscard]] static Common::ResultStr<std::optional<std::string>> Upgrade( std::string_view     text,
                                                                                   const LegacyClasses& classes );

        /// Reads and parses the `.desound` at @p desound.
        [[nodiscard]] static Common::ResultStr<Parsed> ReadFile( const std::filesystem::path& desound );

        /// The `.desound` text naming @p source under @p guid, played in @p soundClass with @p loading. Refuses a
        /// null GUID, an empty source and a null class.
        [[nodiscard]] static Common::ResultStr<std::string> Write( const Common::Content::AssetGuid& guid,
                                                                   std::string_view                  source,
                                                                   const Audio::SoundClassId&        soundClass,
                                                                   Audio::SoundLoading               loading );

        /// What a voice of the sound @p guid plays. Refuses an unknown GUID and every Parse refusal, by name.
        [[nodiscard]] static Common::ResultStr<Audio::SoundSource>
        Resolve( const Common::Content::AssetGuid& guid );

        /// The same, for a reference held as the GUID's handle.
        [[nodiscard]] static Common::ResultStr<Audio::SoundSource> Resolve( AssetHandle handle );

        /// Writes a `.desound` beside its source (creating the directory).
        static Common::BoolResultStr Save( const Common::Filepath&           filepath,
                                           const Common::Content::AssetGuid& guid, std::string_view source,
                                           const Audio::SoundClassId& soundClass, Audio::SoundLoading loading );

    private:
        Common::Content::AssetGuid m_Guid;
        std::filesystem::path      m_SourceFile;
        Audio::SoundClassId        m_Class; // null until LoadFromFile
        Audio::SoundLoading        m_Loading = Audio::SoundLoading::Resident;
        bool                       m_Ready   = false;
    };
} // namespace Desert::Assets

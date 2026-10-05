#pragma once

#include <Engine/Assets/AssetBase.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    inline constexpr std::string_view kSoundExtension = ".desound";
    inline constexpr std::string_view kSoundKind      = "Sound";

    /**
     * @brief A SOUND (`.desound`) — UE's USoundWave: the identity every sound reference names.
     *
     * THE FILE: a text asset header (Kind "Sound", the GUID) and `Source`, the imported audio file's name
     * RELATIVE TO THE `.desound` (UE keeps the imported source beside the asset the same way). A track or a
     * component names the GUID; the path lives here and nowhere else, so moving the pair moves no reference.
     * The bytes are not read at load: AudioEngine streams the source when a voice starts.
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

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Sound;
        }

        struct Parsed
        {
            Common::Content::AssetGuid Guid;
            std::string                Source; ///< as written: relative to the `.desound`
        };

        /// Refuses a header of another kind, a null/malformed GUID and an empty Source, by name.
        [[nodiscard]] static Common::ResultStr<Parsed> Parse( std::string_view text );

        /// The `.desound` text naming @p source under @p guid. Refuses a null GUID and an empty source.
        [[nodiscard]] static Common::ResultStr<std::string> Write( const Common::Content::AssetGuid& guid,
                                                                   std::string_view                  source );

        /// The audio file the sound @p guid names: the registry row of the GUID (through redirectors), its
        /// `.desound` read and parsed, its Source resolved beside it. Refuses an unknown GUID and every Parse
        /// refusal, by name. What a sound REFERENCE resolves through — the path is never stored by a user.
        [[nodiscard]] static Common::ResultStr<std::filesystem::path>
        ResolveSourceFile( const Common::Content::AssetGuid& guid );

        /// Writes a `.desound` beside its source (creating the directory). Static: saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath& filepath, const Common::Content::AssetGuid& guid,
                                           std::string_view source );

    private:
        Common::Content::AssetGuid m_Guid;
        std::filesystem::path      m_SourceFile;
        bool                       m_Ready = false;
    };
} // namespace Desert::Assets

#pragma once

#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/AssetBase.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <string>

namespace Desert::Assets
{
    /**
     * @brief A LEVEL SEQUENCE (`.dseq`) — UE's ULevelSequence: a `Timeline::Sequence` (Host = LevelSequence)
     * placed in a scene by a `LevelSequenceComponent`.
     *
     * THE FILE IS THE TMLN BLOCK ITSELF (Timeline/Hosts.hpp: "a text-header asset whose body is the TMLN
     * block"): the block's first member is already a text asset header, which `WriteSequence` leaves without
     * identity. `Save` stamps that header with Kind "LevelSequence" and the asset's GUID, so the registry scan,
     * `ReadTextAssetIdentity` and the dependency walk read a `.dseq` the way they read every text asset, and
     * `ReadSequence` reads the same bytes back (it reads the version and ignores Kind/Guid). One document, one
     * writer, no second format.
     */
    class LevelSequenceAsset final : public AssetBase
    {
    public:
        LevelSequenceAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Animation::Timeline::Sequence& GetSequence() const
        {
            return m_Sequence;
        }

        /// The Sequencer's document edits the LOADED sequence in place (UE: the Sequencer edits the
        /// ULevelSequence object), so every placed LevelSequenceComponent plays the edit at once — its
        /// Evaluator rebuilds on `Revision`, which every edit bumps. Saving writes this same object.
        [[nodiscard]] Animation::Timeline::Sequence& EditSequence()
        {
            return m_Sequence;
        }

        [[nodiscard]] std::string GetDisplayName() const
        {
            return m_Metadata.Filepath.stem().string();
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::LevelSequence;
        }

        /// The `.dseq` text of @p sequence under identity @p guid. Refuses a sequence whose host is not
        /// LevelSequence and a null GUID, by name.
        [[nodiscard]] static Common::ResultStr<std::string> Write( const Animation::Timeline::Sequence& sequence,
                                                                   const Common::Content::AssetGuid&    guid );

        /// The sequence and GUID of `.dseq` text. Refuses a header of another kind, a missing GUID and every
        /// ReadSequence refusal, by name.
        struct Parsed
        {
            Common::Content::AssetGuid    Guid;
            Animation::Timeline::Sequence Sequence;
        };
        [[nodiscard]] static Common::ResultStr<Parsed> Parse( std::string_view text );

        /// Writes a sequence to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&              filepath,
                                           const Animation::Timeline::Sequence& sequence,
                                           const Common::Content::AssetGuid&    guid );

    private:
        Common::Content::AssetGuid    m_Guid;
        Animation::Timeline::Sequence m_Sequence;
        bool                          m_Ready = false;
    };
} // namespace Desert::Assets

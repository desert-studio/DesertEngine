#pragma once

#include <Engine/Assets/Serialization/Retarget.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Editor
{
    class CommandHistory;

    /**
     * @brief What the `.retarget` window edits, without the window: the authored data, its saved twin, the
     *        undo records, Auto-map and the verdict of the runtime's own chain resolver. UE: the state an
     *        `FIKRetargetEditorController` edits on a `UIKRetargeter`, minus the viewports.
     *
     * SPLIT OUT FOR THE StaticMeshViewerBase REASON: the window needs two PreviewViewports and a device, and
     * the suite (RetargetDocument) needs none. Every edit the window makes goes through here, so the suite
     * exercises the edit the user makes rather than a copy of it.
     *
     * ONE UNDO RECORD = ONE WHOLE-DATA SNAPSHOT PAIR. A retarget is a few hundred bytes of names; a record per
     * field would be a second description of the format that has to grow with it. The records name THIS model
     * as their EditedObject and the destructor drops them (CommandHistory::DropFor), so closing the window
     * cannot leave a record that writes into freed memory.
     *
     * THE VERDICT IS THE RUNTIME'S. Whether a chain resolves is answered by `Retargeter::Initialize` — which
     * runs `ResolveChains` — on the same skeletons, never by a second resolver here that could agree with the
     * window and disagree with the game.
     */
    class RetargetDocumentModel
    {
    public:
        using Data  = Assets::Serialization::RetargetAssetData;
        using Chain = Assets::Serialization::RetargetChainData;

        RetargetDocumentModel( Data data, CommandHistory& history );
        ~RetargetDocumentModel();

        RetargetDocumentModel( const RetargetDocumentModel& )            = delete;
        RetargetDocumentModel& operator=( const RetargetDocumentModel& ) = delete;

        [[nodiscard]] const Data& GetData() const
        {
            return m_Data;
        }

        [[nodiscard]] bool IsDirty() const
        {
            return m_Data != m_Saved;
        }

        /// Bumped by every change, undo and redo included: the window recomputes its verdicts when it moves.
        [[nodiscard]] uint64_t GetRevision() const
        {
            return m_Revision;
        }

        /// Replaces chain @p index, as one undo record. Refuses an index past the list, an empty name and a
        /// name another chain already has (a chain is addressed by name in every message the resolver writes).
        [[nodiscard]] Common::BoolResultStr EditChain( size_t index, Chain chain );
        [[nodiscard]] Common::BoolResultStr AddChain( std::string name );
        [[nodiscard]] Common::BoolResultStr RemoveChain( size_t index );

        /**
         * @brief UE's Auto-map Chains: fills every chain's TARGET start and end from its source bones, as one
         *        undo record. Returns how many chains were mapped.
         *
         * A source bone's target is, in order: the target bone a `BoneRenames` row pairs it with (the file's
         * own word), the target bone of the same name (the pairing rule `Retargeter::BuildPairings` uses), or
         * the ONE target bone whose name matches once the rig namespace (`mixamorig:`) and case/separators are
         * ignored. Two candidates is no answer, and a chain with an end that finds none keeps its old target
         * bones — its ⚠ in the window is the report, not a guessed bone.
         */
        [[nodiscard]] Common::ResultStr<size_t> AutoMap( const Animation::Skeleton& source,
                                                         const Animation::Skeleton& target );

        /// The whole retarget, as the runtime would build it (`Retargeter::Initialize`). Success, or its error.
        [[nodiscard]] Common::BoolResultStr Validate( const Animation::Skeleton& source,
                                                      const Animation::Skeleton& target ) const;

        /// Per chain, in list order: empty when the runtime resolves that chain alone, else its reason.
        [[nodiscard]] std::vector<std::string> ChainProblems( const Animation::Skeleton& source,
                                                              const Animation::Skeleton& target ) const;

        /// Writes the data to @p path; on success the data is the new saved state (IsDirty() == false).
        [[nodiscard]] Common::BoolResultStr Save( const std::filesystem::path& path );

        /// Back to the saved state, and this model's undo records dropped (they describe edits that are gone).
        void Discard();

    private:
        friend class RetargetEditRecord;

        void Commit( Data before, std::string label );
        void Restore( const Data& data );

        Data            m_Data;
        Data            m_Saved;
        CommandHistory& m_History;
        uint64_t        m_Revision = 0;
    };
} // namespace Desert::Editor

#pragma once

/**
 * ONE UNDO PATH FOR EVERY EDIT OF AN ANIM GRAPH: A TRANSACTION OVER THE `.danimgraph`'s `AnimGraph`.
 *
 * UE's AnimGraph editor opens an `FScopedTransaction` on the AnimBlueprint for every edit of its graphs — a
 * node added, a wire drawn, a node dragged, a pin default typed, a state or a parameter added — and one undo
 * entry holds the object before and after. This is that, over the editor's ONE `CommandHistory`, shaped as
 * `SequenceEdit.hpp` is for a Timeline::Sequence: a by-value snapshot when the transaction opens, a diff when
 * it closes, AT MOST ONE entry per interaction (a drag of forty frames is one entry, a click that changed
 * nothing is none).
 *
 * WHY THE WHOLE GRAPH AND NOT A DELTA: an anim graph is a few kilobytes (nodes, states, wires by name), a
 * rename rewrites every wire that named the node, and a node deletion unwires its readers. A snapshot
 * restores all of that without knowing it was derived; an inverse per edit kind would be a census of edit
 * kinds that a new one falls out of.
 *
 * "CHANGED" IS THE STORED FORM: two graphs are the same edit state when `Serialize` writes the same bytes
 * (the header forced equal first — no edit touches it, and a never-written graph would mint a new GUID per
 * call). So a field added to any aggregate of the graph cannot fall out of "did this interaction change
 * anything": the serializer is the census.
 *
 * THE OWNER RESOLVES AT EVERY USE (the asset by handle), so an entry survives a reload of the file; the
 * restore calls `AfterRestore` (the asset's `MarkEdited`), which is what makes every entity on the graph
 * re-sync and every open window re-read it.
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Desert::Editor
{
    /// Whether @p a and @p b are the same stored graph: `Serialize` writes the same bytes, header aside.
    [[nodiscard]] bool SameStoredGraph( const Animation::Graph::AnimGraph& a,
                                        const Animation::Graph::AnimGraph& b );

    /**
     * @brief Whose graph an edit is on, resolved at every use (UE: the transacted UAnimBlueprint).
     */
    struct AnimGraphOwner
    {
        /// What `CommandHistory::DropFor` matches. Compared, never read.
        const void* Identity = nullptr;
        /// The live graph, or null when the owner is gone (the entry then refuses to apply).
        std::function<Animation::Graph::AnimGraph*()> Resolve;
        /// Runs after Undo/Redo wrote the graph: the asset's revision bump.
        std::function<void()> AfterRestore;
        /// True when `Resolve` dereferences a captured raw address.
        bool        Volatile = true;
        std::string Name;
    };

    /**
     * @brief ONE interaction's net effect on one anim graph.
     */
    class AnimGraphEditCommand final : public ICommand
    {
    public:
        AnimGraphEditCommand( AnimGraphOwner owner, Animation::Graph::AnimGraph before,
                              Animation::Graph::AnimGraph after );

        bool               Undo() override;
        bool               Redo() override;
        [[nodiscard]] bool IsVolatile() const override
        {
            return m_Owner.Volatile;
        }
        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Owner.Identity;
        }
        [[nodiscard]] std::string GetLabel() const override;

    private:
        [[nodiscard]] bool Apply( const Animation::Graph::AnimGraph& value ) const;

        AnimGraphOwner              m_Owner;
        Animation::Graph::AnimGraph m_Before;
        Animation::Graph::AnimGraph m_After;
    };

    /**
     * @brief The interaction boundary: one open transaction = at most one `AnimGraphEditCommand`.
     *
     * TWO DRIVERS, ONE TRANSACTION, as `SequenceEditTransaction`'s. `Begin`/`End` around an edit whose edges
     * the caller owns (a document action, a menu entry: `Scope` below), and `Observe` once per frame for
     * the canvas and the side panel, whose edges are "a mouse button or an item is held". The BASELINE is
     * the graph as of the last settled frame, so the rising edge opens on the true before even when the
     * widget wrote on the frame it was pressed.
     *
     * A settled frame whose graph moved without a transaction (a key press: Delete on the canvas) is ONE
     * entry too. A move the history made itself (an Undo, a Redo, another transaction's push) is not an
     * edit of this window's: the baseline follows it, and a gesture open across it is dropped.
     */
    class AnimGraphEditTransaction
    {
    public:
        /// Open on @p owner. Refuses while one is open and an owner that does not resolve.
        [[nodiscard]] Common::BoolResultStr Begin( const AnimGraphOwner& owner );
        /// Close and push AT MOST ONE entry: 0 when nothing stored changed, 1 otherwise.
        [[nodiscard]] Common::ResultStr<uint32_t> End();
        /// Close and push nothing.
        void Cancel();
        /**
         * @brief One call per frame, after everything that edits the graph. Returns entries pushed (0 or 1).
         * @param revision the asset's revision: a settled frame compares only when it moved.
         */
        [[nodiscard]] uint32_t Observe( const AnimGraphOwner& owner, uint32_t revision, bool held );

        [[nodiscard]] bool Open() const
        {
            return m_Open;
        }

        /// An edit with its own edges: opens unless a gesture already holds the transaction (the edit then
        /// joins it — a menu entry chosen with the mouse is part of that click), closes on destruction.
        class Scope
        {
        public:
            Scope( AnimGraphEditTransaction& transaction, const AnimGraphOwner& owner );
            ~Scope();
            Scope( const Scope& )            = delete;
            Scope& operator=( const Scope& ) = delete;

        private:
            AnimGraphEditTransaction& m_Transaction;
            bool                      m_Began = false;
        };

    private:
        void Rebase( const Animation::Graph::AnimGraph& graph, uint32_t revision );

        AnimGraphOwner                             m_Owner;
        bool                                       m_Open = false;
        std::optional<Animation::Graph::AnimGraph> m_Before;
        uint64_t                                   m_HistoryAtOpen = 0;
        // The settled state the next gesture opens on.
        std::optional<Animation::Graph::AnimGraph> m_Baseline;
        const void*                                m_BaselineOwner    = nullptr;
        uint32_t                                   m_BaselineRevision = 0;
        uint64_t                                   m_HistorySeen      = 0;
    };
} // namespace Desert::Editor

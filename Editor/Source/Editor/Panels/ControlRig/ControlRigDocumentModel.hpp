#pragma once

/**
 * THE .derig DOCUMENT'S MODEL — what the Control Rig editor edits, with no ImGui in it.
 *
 * The window (ControlRigDocument) is three views over ONE value: the file form `ControlRigData` this rig
 * was read as. Every edit the window makes — a control added, a node wired, a colour picked in the
 * inspector — is a function here that builds the next value, and that value is installed through ONE door
 * (`Commit`), which is also the one place an undo record is pushed. So "the canvas changed the rig but undo
 * does not know" has no route to exist, and a suite can drive the whole document without a window.
 *
 * ── WHAT AN EDIT REFUSES, AND WHAT IT LEAVES TO SAVE ─────────────────────────────────────────────────
 *
 * An edit refuses what can never become a valid rig by more editing: a name that does not exist, a second
 * control or node of the same name, a link between pins of different types, a link that closes a cycle. It
 * does NOT refuse an intermediate state a rigger passes through on the way to a valid one — a node whose
 * output feeds nothing yet is exactly what the canvas holds between "drop a Get Control" and "drag its
 * wire". Those are what `ValidateControlRigData` answers, which the window shows on its status line and
 * `Save` refuses on — the file format refuses to READ an invalid rig, so writing one would be a file the
 * next session cannot open.
 *
 * ── RENAME CARRIES ITS REFERENCES ────────────────────────────────────────────────────────────────────
 *
 * The file binds everything by NAME (ControlRig.hpp's file note). Renaming a control therefore rewrites
 * every place that names it — a child's parent space, a bone drive, a graph node's target — in the same
 * record, or the rename is a dangling reference the validator reports one save later.
 */

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Animation/Rig/RigGraph.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    class ControlRigDocumentModel
    {
    public:
        using Data = Assets::Serialization::ControlRigData;

        /// Reads @p path; refused with the parser's reason. The model writes its undo records into
        /// @p history and drops them again when it dies (CommandHistory::DropFor).
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<ControlRigDocumentModel>>
        Open( const std::filesystem::path& path, CommandHistory& history );

        ControlRigDocumentModel( std::filesystem::path path, Data loaded, CommandHistory& history );
        ~ControlRigDocumentModel();

        ControlRigDocumentModel( const ControlRigDocumentModel& )            = delete;
        ControlRigDocumentModel& operator=( const ControlRigDocumentModel& ) = delete;

        [[nodiscard]] const Data& GetData() const
        {
            return m_Data;
        }
        [[nodiscard]] const std::filesystem::path& GetPath() const
        {
            return m_Path;
        }
        /// Differs from what is on disk (equality, not a flag: undo back to the saved value is clean).
        [[nodiscard]] bool IsDirty() const
        {
            return m_Data != m_Saved;
        }
        /// Moves on every installed value — commit, undo, redo — so a view can cache by it.
        [[nodiscard]] uint64_t GetRevision() const
        {
            return m_Revision;
        }
        /// `ValidateControlRigData` on the current value: what Save would refuse with, live.
        [[nodiscard]] Common::BoolResultStr Validate() const;

        /// Writes the current value to the document's path. Refused (and nothing written) while invalid.
        [[nodiscard]] Common::BoolResultStr Save();

        // ── Rig elements ───────────────────────────────────────────────────────────────────────────
        [[nodiscard]] Common::BoolResultStr AddControl( const std::string& name );
        /// Removes the control and every reference to it: children re-parented to what it followed is a
        /// guess, so a child's space naming it is REMOVED (an empty space list means the component), its
        /// drives go, and graph nodes targeting it go with their links.
        [[nodiscard]] Common::BoolResultStr RemoveControl( const std::string& name );
        /// The inspector's write: the whole element. A changed Name renames every reference.
        [[nodiscard]] Common::BoolResultStr SetControl( const std::string&                               name,
                                                        const Assets::Serialization::ControlElementData& value );
        /// The control's bone drive: empty @p bone removes it.
        [[nodiscard]] Common::BoolResultStr SetDrive( const std::string& control, const std::string& bone );
        /// The control's limited channels, whole (UE `FRigControlLimitEnabled` per axis): an unknown channel,
        /// a channel named twice, or Min > Max is refused before anything is recorded.
        [[nodiscard]] Common::BoolResultStr
        SetLimits( const std::string&                                          control,
                   const std::vector<Assets::Serialization::ControlLimitData>& limits );

        // ── Which solve event the graph edits address ──────────────────────────────────────────────
        [[nodiscard]] Animation::RigEvent GetEvent() const
        {
            return m_Event;
        }
        /// A view choice (UE's event tabs), not an edit: no undo record and the document stays clean.
        void SetEvent( Animation::RigEvent event )
        {
            m_Event = event;
            ++m_Revision;
        }
        /// The selected event's graph, or null while the rig defines none for it.
        [[nodiscard]] const Assets::Serialization::RigGraphData* GetGraph() const;

        // ── Graph of the selected event ────────────────────────────────────────────────────────────
        /// Adds a node of @p kind at canvas @p position; every input starts as its type's identity literal.
        /// The event's graph is created by its first node. Answers the node's name.
        [[nodiscard]] Common::ResultStr<std::string>
        AddNode( Animation::RigNodeKind kind, const std::string& target,
                 const Assets::Serialization::RigNodePositionData& position );
        /// Moves a node on the canvas — one undo record per drag (the view commits on release).
        [[nodiscard]] Common::BoolResultStr          MoveNode( const std::string&                                node,
                                                               const Assets::Serialization::RigNodePositionData& position );
        [[nodiscard]] Common::BoolResultStr          RemoveNode( const std::string& node );
        [[nodiscard]] Common::BoolResultStr SetNodeTarget( const std::string& node, const std::string& target );
        [[nodiscard]] Common::BoolResultStr SetNodeSpace( const std::string&         node,
                                                          Animation::RigControlSpace space );
        /// Wires @p fromNode.@p fromPin (an output) into @p toNode.@p toPin (an input).
        [[nodiscard]] Common::BoolResultStr Connect( const std::string& fromNode, const std::string& fromPin,
                                                     const std::string& toNode, const std::string& toPin );
        /// Cuts the wire into @p toNode.@p toPin; the pin goes back to its type's identity literal.
        [[nodiscard]] Common::BoolResultStr Disconnect( const std::string& toNode, const std::string& toPin );
        /// The literal of an unwired input (exactly one of the value fields of @p literal set, and it is the
        /// field of the pin's type — a Vec3 typed into a Float pin is refused here, not at Save).
        [[nodiscard]] Common::BoolResultStr SetLiteral( const std::string&                              node,
                                                        const Assets::Serialization::RigGraphInputData& literal );

        /// Installs @p after as one undo record named @p label. The one door every edit goes through.
        [[nodiscard]] Common::BoolResultStr Commit( std::string label, Data after );

        /// Called by the undo record: installs a value without a record.
        void Restore( const Data& value );

    private:
        std::filesystem::path m_Path;
        Data                  m_Data;
        Data                  m_Saved;
        CommandHistory&       m_History;
        uint64_t              m_Revision = 0;
        Animation::RigEvent   m_Event    = Animation::RigEvent::Forwards;
    };

    /// The identity literal of an input pin of @p type (0, zero vector, identity rotation/transform).
    [[nodiscard]] Assets::Serialization::RigGraphInputData DefaultRigInput( std::string_view        pin,
                                                                            Animation::RigValueKind type );
} // namespace Desert::Editor

#pragma once

#include <Engine/Core/Serialize/EditStamps.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // One undoable editor action. Commands should identify their targets by STABLE ids (entity UUIDs)
    // so an entry stays valid regardless of what happened to the scene in between; Undo/Redo return
    // false when the target no longer exists, and the stack silently discards such stale entries.
    //
    // Commands that instead hold raw pointers into live objects (the reflected-property byte edits)
    // must report IsVolatile() — the stack drops them whenever their target may have died (selection
    // change, entity creation/destruction), because a stale pointer can't even be *detected*, only
    // crashed into.
    class ICommand
    {
    public:
        virtual ~ICommand() = default;

        virtual bool Undo() = 0;
        virtual bool Redo() = 0;

        virtual bool IsVolatile() const
        {
            return false;
        }

        // The object this record writes through a raw pointer when it is NOT a scene entity (an asset's payload
        // an asset editor edits), or null. CommandHistory::DropFor forgets every record of one subject when
        // its editor closes: the history is process-wide and outlives the window, the payload need not.
        [[nodiscard]] virtual const void* EditedObject() const
        {
            return nullptr;
        }

        // Short human-readable name for the History panel (e.g. "Move", "Rename", "Delete").
        virtual std::string GetLabel() const
        {
            return "Edit";
        }

    private:
        // WHICH ENTITY FILES THIS RECORD MADE DIRTY (WP17, Core/Serialize/EditStamps.hpp) - UE's transaction
        // remembering each package's dirty state. Kept and applied by CommandHistory alone: a command never sees
        // them, and a record that named no entities (and is not an asset edit) marks the whole scene.
        friend class CommandHistory;
        std::weak_ptr<::Desert::Core::IEditStamps>      m_Ledger;
        std::vector<::Desert::Core::IEditStamps::Stamp> m_Stamps;
        bool                                            m_StampsWhole = false;
    };

    // The single editor-wide undo/redo stack: reflected-property edits, gizmo transforms and structural
    // scene changes (create/delete/reparent/duplicate/rename) all land here, so Ctrl+Z walks back through
    // everything in the order it actually happened.
    class CommandHistory
    {
    public:
        static CommandHistory& Get()
        {
            static CommandHistory s_Instance;
            return s_Instance;
        }

        // Reflected-property byte edit (see PropertyEditorBuilder): before/after bytes at a raw field
        // address. Pointer-based -> volatile (dropped when the edited object may have died).
        //
        // ONLY FOR TRIVIALLY COPYABLE FIELDS. Restoring is a memcpy, so a field that owns heap — a
        // std::string — must not come here: the copy carries the string's BUFFER POINTER, and putting it
        // back hands the live object memory the allocator has already reclaimed. Use PushString for
        // those. Which field types may take which route is decided once, in
        // Editor/Panels/PropertyEditor/PropertyUndoPolicy.hpp, and asserted per type by
        // Desert/Tests/Editor/PropertyUndoPolicy.
        void Push( void* target, const void* oldBytes, const void* newBytes, std::size_t size )
        {
            PushFieldEdit( std::make_unique<ByteCommand>( target, oldBytes, newBytes, size ) );
        }

        // A reflected field edit that is neither bytes nor a string - one element of a std::vector field, restored
        // through the field's typed container accessors (ReflectedFieldText's `Name[i]`). Recorded like Push: as
        // the scoped entity's edit when a FieldSubject is alive.
        void PushFieldCommand( std::unique_ptr<ICommand> command )
        {
            PushFieldEdit( std::move( command ) );
        }

        // Reflected-property STRING edit: before/after VALUES, restored by assignment. The value and the
        // object's representation are different things for a std::string, and only the value survives
        // being stored (see Push above).
        void PushString( std::string* target, std::string oldValue, std::string newValue )
        {
            PushFieldEdit(
                 std::make_unique<StringCommand>( target, std::move( oldValue ), std::move( newValue ) ) );
        }

        // THE ENTITY A FIELD EDIT BELONGS TO (WP17). The reflected-property builder writes raw field addresses
        // and cannot say whose they are; the panel that draws an entity's components can. While a scope is
        // alive, Push/PushString are recorded as edits of its entity (PushEntityEdit) - only that entity's file
        // is rewritten - instead of edits of the whole scene. Scopes nest; the innermost names the entity.
        class FieldSubject
        {
        public:
            explicit FieldSubject( Common::UUID entity )
            {
                Get().m_FieldSubjects.push_back( entity );
            }
            ~FieldSubject()
            {
                Get().m_FieldSubjects.pop_back();
            }
            FieldSubject( const FieldSubject& )            = delete;
            FieldSubject& operator=( const FieldSubject& ) = delete;
        };

        // The entity ledger of the scene the scene commands edit (Commands::SetContext binds it). Held weakly: a
        // record outliving its scene stamps nothing.
        void BindPackages( std::weak_ptr<::Desert::Core::IEditStamps> ledger )
        {
            m_Packages = std::move( ledger );
        }

        // A record that names the entities whose records it changed: only their files are rewritten by the next
        // save, and its undo/redo puts their revisions back (WP17). The entities created, deleted or reparented
        // need not be named - the save reads that off the hierarchy.
        void PushEntityEdit( std::unique_ptr<ICommand> command, const std::vector<Common::UUID>& entities )
        {
            command->m_Ledger = m_Packages;
            if ( const auto ledger = m_Packages.lock() )
                for ( const Common::UUID id : entities )
                    command->m_Stamps.push_back( ledger->Touch( id ) );
            Record( std::move( command ) );
        }

        // A record that names no entities: an asset edit (EditedObject) touches no scene file; anything else may
        // have changed any record, so the next save of the scene is whole.
        void PushCommand( std::unique_ptr<ICommand> command )
        {
            if ( command->EditedObject() == nullptr )
            {
                command->m_Ledger      = m_Packages;
                command->m_StampsWhole = true;
                if ( const auto ledger = m_Packages.lock() )
                    ledger->TouchAll();
            }
            Record( std::move( command ) );
        }

    private:
        void PushFieldEdit( std::unique_ptr<ICommand> command )
        {
            if ( m_FieldSubjects.empty() )
                PushCommand( std::move( command ) );
            else
                PushEntityEdit( std::move( command ), { m_FieldSubjects.back() } );
        }

        static void ApplyStamps( const ICommand& command, bool undo )
        {
            const auto ledger = command.m_Ledger.lock();
            if ( !ledger )
                return;
            if ( command.m_StampsWhole )
            {
                ledger->TouchAll();
                return;
            }
            if ( undo )
                for ( const auto& m_Stamp : std::ranges::reverse_view( command.m_Stamps ) )
                    ledger->Restore( m_Stamp.Id, m_Stamp.Before );
            else
                for ( const auto& stamp : command.m_Stamps )
                    ledger->Restore( stamp.Id, stamp.After );
        }

        void Record( std::unique_ptr<ICommand> command )
        {
            // A new edit invalidates the redo branch.
            m_Redo.clear();
            m_Undo.push_back( std::move( command ) );
            if ( m_Undo.size() > kMaxEntries )
                m_Undo.erase( m_Undo.begin() );
            Bump();
        }

    public:
        /// The last two entries become ONE, undone newest-first and redone oldest-first. For a single
        /// user action whose two halves are recorded by two owners a frame apart: an auto-keyed control
        /// edit is the control's pose entry (RecordControlDrag) and the Sequencer's key entry, and one
        /// Ctrl+Z has to take back both, as UE's one transaction does. False (and nothing changes) when
        /// fewer than two entries exist.
        bool JoinLastTwo()
        {
            if ( m_Undo.size() < 2 )
            {
                return false;
            }
            std::unique_ptr<ICommand> second = std::move( m_Undo.back() );
            m_Undo.pop_back();
            std::unique_ptr<ICommand> first = std::move( m_Undo.back() );
            m_Undo.pop_back();
            auto joined = std::make_unique<JoinedCommand>( std::move( first ), std::move( second ) );
            joined->m_Ledger =
                 joined->First().m_Ledger.expired() ? joined->Second().m_Ledger : joined->First().m_Ledger;
            joined->m_StampsWhole = joined->First().m_StampsWhole || joined->Second().m_StampsWhole;
            joined->m_Stamps      = joined->First().m_Stamps;
            joined->m_Stamps.insert( joined->m_Stamps.end(), joined->Second().m_Stamps.begin(),
                                     joined->Second().m_Stamps.end() );
            m_Undo.push_back( std::move( joined ) );
            Bump();
            return true;
        }

        /// The second half of one user action joins the first (`JoinLastTwo`) only when it provably follows
        /// it: the first half's entry was pushed at revision @p first, the history still stood there when
        /// the second half opened at @p opened (nothing recorded, undone or redone in between), and the
        /// second half pushed exactly one entry since. An actor gizmo drag with REC on is UE's one
        /// FScopedTransaction around the move AND its auto-key; here the move is the gizmo's entry and the
        /// key the Sequencer's. False (and nothing changes) on anything else — two honest entries beat one
        /// that swallowed an unrelated edit.
        bool JoinFollowUp( uint64_t first, uint64_t opened )
        {
            if ( opened != first || m_Revision != opened + 1 )
            {
                return false;
            }
            return JoinLastTwo();
        }

        bool Undo()
        {
            // Stale entries (target entity gone) report failure — discard them and keep walking down.
            while ( !m_Undo.empty() )
            {
                std::unique_ptr<ICommand> cmd = std::move( m_Undo.back() );
                m_Undo.pop_back();
                if ( cmd->Undo() )
                {
                    // Every step the history walks is named: a script's Undo that took back a selection
                    // change rather than the edit it meant is read off the log, not guessed from a frame.
                    LOG_INFO( "[History] undo '{0}' ({1} left)", cmd->GetLabel(), m_Undo.size() );
                    ApplyStamps( *cmd, true );
                    m_Redo.push_back( std::move( cmd ) );
                    Bump();
                    return true;
                }
                LOG_WARN( "[History] '{0}' could not be undone (its target is gone) - dropped", cmd->GetLabel() );
            }
            return false;
        }

        bool Redo()
        {
            while ( !m_Redo.empty() )
            {
                std::unique_ptr<ICommand> cmd = std::move( m_Redo.back() );
                m_Redo.pop_back();
                if ( cmd->Redo() )
                {
                    LOG_INFO( "[History] redo '{0}' ({1} left)", cmd->GetLabel(), m_Redo.size() );
                    ApplyStamps( *cmd, false );
                    m_Undo.push_back( std::move( cmd ) );
                    Bump();
                    return true;
                }
            }
            return false;
        }

        // Monotonic edit counter (bumped by every push/undo/redo). "Unsaved changes" = the revision moved
        // since the last save marker; Clear() does NOT bump it (loading a scene isn't an edit).
        uint64_t Revision() const
        {
            return m_Revision;
        }

        // THE DOCUMENT'S REVISION (PIE-UNDO-KEEP): Revision() less every bump a Play session made - its records, its
        // undo/redo, the barrier itself. The played world is not the document (UE: PIE never dirties the level's
        // package), so "unsaved changes" (SceneFiles) compares THIS one: a Play/Stop round trip leaves a saved scene
        // clean and a dirty one dirty. Never decreases; Revision() stays the counter for "did the history change".
        uint64_t DocumentRevision() const
        {
            return m_Revision - m_SessionRevisions;
        }

        // Drops only the pointer-based (volatile) entries; UUID-addressed structural commands survive.
        void DropVolatile()
        {
            auto drop = []( std::vector<std::unique_ptr<ICommand>>& stack )
            {
                std::erase_if( stack, []( const std::unique_ptr<ICommand>& c ) { return c->IsVolatile(); } );
            };
            drop( m_Undo );
            drop( m_Redo );
            for ( Barrier& barrier : m_Barriers )
            {
                drop( barrier.Undo );
                drop( barrier.Redo );
            }
        }

        // Forgets every record (undo and redo) that writes into @p object. An asset editor calls it when it
        // closes: UE's transactions keep their object alive; ours hold a raw pointer into a payload the asset
        // manager may evict once the window's root pin is gone, so the records go with the window.
        void DropFor( const void* object )
        {
            if ( object == nullptr )
                return;
            auto drop = [object]( std::vector<std::unique_ptr<ICommand>>& stack )
            {
                std::erase_if( stack, [object]( const std::unique_ptr<ICommand>& c )
                               { return c->EditedObject() == object; } );
            };
            drop( m_Undo );
            drop( m_Redo );
            for ( Barrier& barrier : m_Barriers )
            {
                drop( barrier.Undo );
                drop( barrier.Redo );
            }
        }

        // DropFor by a question other than "which object": the records `drops` answers true for, from both
        // stacks. The Animation Editor needs it when its preview ANIMATOR dies (a mesh switch, a released
        // preview) while the clip - the edited object - lives on with its notify and curve records.
        template <typename Predicate>
        size_t DropIf( Predicate drops )
        {
            size_t     dropped = 0;
            const auto drop    = [&]( std::vector<std::unique_ptr<ICommand>>& stack )
            { dropped += std::erase_if( stack, [&]( const std::unique_ptr<ICommand>& c ) { return drops( *c ); } ); };
            drop( m_Undo );
            drop( m_Redo );
            for ( Barrier& barrier : m_Barriers )
            {
                drop( barrier.Undo );
                drop( barrier.Redo );
            }
            return dropped;
        }

        // A PLAY SESSION FENCES THE HISTORY, IT DOES NOT ERASE IT (PIE-UNDO-KEEP). UE sets an undo barrier when PIE
        // starts ("so that transactions prior to PIE can't be undone", UEditorEngine, PlayLevel.cpp:2972) and removes
        // it in EndPlayMap (:677): after Stop, Ctrl+Z walks the editor's history again. Here the played world IS the
        // edit world (the Scene runs; Stop clears it and loads the snapshot back into it), so the fence is stricter
        // than UE's index into one buffer: SetUndoBarrier PARKS both stacks - neither an undo nor a redo of an
        // edit-world record may write into the played world - and the session records onto empty ones.
        // Volatile records must be gone before (DropVolatile): Play relocates every component pool they point into.
        void SetUndoBarrier()
        {
            Barrier barrier;
            barrier.Undo = std::move( m_Undo );
            barrier.Redo = std::move( m_Redo );
            m_Undo.clear();
            m_Redo.clear();
            m_Barriers.push_back( std::move( barrier ) );
            Bump();
            LOG_INFO( "[History] undo barrier set: {0} undo / {1} redo step(s) parked until Stop",
                      m_Barriers.back().Undo.size(), m_Barriers.back().Redo.size() );
        }

        // Ends the innermost session: what it recorded against the played world goes (Stop rebuilds that world from
        // the snapshot, so those records name state the document never had; UE instead resets the WHOLE buffer when
        // a PIE object got into it, PlayLevel.cpp:496), its asset edits stay on top (UE keeps a transaction on a
        // material edited during PIE), and the parked stacks come back under them - the parked redo branch only
        // when the session kept nothing, since a kept edit is newer than every record in it. False when no barrier
        // is up (nothing changes).
        bool RemoveUndoBarrier()
        {
            if ( m_Barriers.empty() )
                return false;
            Bump(); // the session's own bump: the document did not change
            Barrier parked = std::move( m_Barriers.back() );
            m_Barriers.pop_back();
            std::vector<std::unique_ptr<ICommand>> kept;
            std::size_t                            dropped = m_Redo.size();
            for ( std::unique_ptr<ICommand>& command : m_Undo )
            {
                if ( OutlivesPlayedWorld( *command ) )
                    kept.push_back( std::move( command ) );
                else
                    ++dropped;
            }
            const std::size_t keptCount = kept.size();
            m_Undo                      = std::move( parked.Undo );
            m_Redo.clear();
            if ( kept.empty() )
                m_Redo = std::move( parked.Redo );
            for ( std::unique_ptr<ICommand>& command : kept )
                m_Undo.push_back( std::move( command ) );
            if ( m_Undo.size() > kMaxEntries )
                m_Undo.erase( m_Undo.begin(),
                              m_Undo.begin() + static_cast<std::ptrdiff_t>( m_Undo.size() - kMaxEntries ) );
            LOG_INFO( "[History] undo barrier removed: {0} step(s) of the session dropped, {1} kept; "
                      "{2} undo / {3} redo step(s) back",
                      dropped, keptCount, m_Undo.size(), m_Redo.size() );
            return true;
        }

        // How many sessions fence the history (0 outside Play). Undo/Redo stop at the innermost barrier: the live
        // stacks are the session's (UE: "Hit Undo Barrier; can't undo any further", UTransBuffer::CanUndo).
        [[nodiscard]] std::size_t UndoBarrierDepth() const
        {
            return m_Barriers.size();
        }

        // A new document: nothing of the old one can be undone, during a Play session or after it. The barriers
        // themselves stay, so the session that set one still removes it.
        void Clear()
        {
            m_Undo.clear();
            m_Redo.clear();
            for ( Barrier& barrier : m_Barriers )
            {
                barrier.Undo.clear();
                barrier.Redo.clear();
            }
        }

        // Read-only views for the History panel (bottom = oldest, back = the next Undo target).
        const std::vector<std::unique_ptr<ICommand>>& UndoStack() const
        {
            return m_Undo;
        }
        const std::vector<std::unique_ptr<ICommand>>& RedoStack() const
        {
            return m_Redo;
        }

    private:
        class JoinedCommand final : public ICommand
        {
        public:
            JoinedCommand( std::unique_ptr<ICommand> first, std::unique_ptr<ICommand> second )
                 : m_First( std::move( first ) ), m_Second( std::move( second ) )
            {
            }

            bool Undo() override
            {
                const bool second = m_Second->Undo();
                const bool first  = m_First->Undo();
                return first || second;
            }

            bool Redo() override
            {
                const bool first  = m_First->Redo();
                const bool second = m_Second->Redo();
                return first || second;
            }

            [[nodiscard]] bool IsVolatile() const override
            {
                return m_First->IsVolatile() || m_Second->IsVolatile();
            }

            [[nodiscard]] std::string GetLabel() const override
            {
                return m_First->GetLabel() + " + " + m_Second->GetLabel();
            }

            [[nodiscard]] const ICommand& First() const
            {
                return *m_First;
            }
            [[nodiscard]] const ICommand& Second() const
            {
                return *m_Second;
            }

        private:
            std::unique_ptr<ICommand> m_First;
            std::unique_ptr<ICommand> m_Second;
        };

        class ByteCommand final : public ICommand
        {
        public:
            ByteCommand( void* target, const void* oldBytes, const void* newBytes, std::size_t size )
                 : m_Target( target ), m_Size( size )
            {
                m_Old.assign( static_cast<const uint8_t*>( oldBytes ),
                              static_cast<const uint8_t*>( oldBytes ) + size );
                m_New.assign( static_cast<const uint8_t*>( newBytes ),
                              static_cast<const uint8_t*>( newBytes ) + size );
            }

            bool Undo() override
            {
                std::memcpy( m_Target, m_Old.data(), m_Size );
                return true;
            }

            bool Redo() override
            {
                std::memcpy( m_Target, m_New.data(), m_Size );
                return true;
            }

            bool IsVolatile() const override
            {
                return true;
            }

            std::string GetLabel() const override
            {
                return "Property edit";
            }

        private:
            void*                m_Target = nullptr;
            std::size_t          m_Size   = 0;
            std::vector<uint8_t> m_Old;
            std::vector<uint8_t> m_New;
        };

        // The string counterpart of ByteCommand. Same volatility (it holds a raw pointer into a live
        // component) and the same label, because to the user it is the same action — only the way the
        // state is held differs, and it has to differ: see PushString above.
        class StringCommand final : public ICommand
        {
        public:
            StringCommand( std::string* target, std::string oldValue, std::string newValue )
                 : m_Target( target ), m_Old( std::move( oldValue ) ), m_New( std::move( newValue ) )
            {
            }

            bool Undo() override
            {
                *m_Target = m_Old;
                return true;
            }

            bool Redo() override
            {
                *m_Target = m_New;
                return true;
            }

            bool IsVolatile() const override
            {
                return true;
            }

            std::string GetLabel() const override
            {
                return "Property edit";
            }

        private:
            std::string* m_Target = nullptr;
            std::string  m_Old;
            std::string  m_New;
        };

        static constexpr size_t kMaxEntries = 256;

        std::vector<std::unique_ptr<ICommand>> m_Undo;
        std::vector<std::unique_ptr<ICommand>> m_Redo;
        std::weak_ptr<::Desert::Core::IEditStamps> m_Packages;
        std::vector<Common::UUID>                  m_FieldSubjects; // FieldSubject scopes, innermost last
        uint64_t                               m_Revision = 0;

        // One Play session's fence: the stacks it parked (SetUndoBarrier), innermost last.
        struct Barrier
        {
            std::vector<std::unique_ptr<ICommand>> Undo;
            std::vector<std::unique_ptr<ICommand>> Redo;
        };
        std::vector<Barrier> m_Barriers;
        uint64_t             m_SessionRevisions = 0; // bumps made while a barrier was up (DocumentRevision)

        void Bump()
        {
            ++m_Revision;
            if ( !m_Barriers.empty() )
                ++m_SessionRevisions;
        }

        // A record the end of a Play session keeps: an asset edit (it names no scene file) that holds no address
        // inside the world Stop rebuilds. Everything else a session recorded edits the played world.
        static bool OutlivesPlayedWorld( const ICommand& command )
        {
            return command.EditedObject() != nullptr && !command.IsVolatile();
        }
    };
} // namespace Desert::Editor

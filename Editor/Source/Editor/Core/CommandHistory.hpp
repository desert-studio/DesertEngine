#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
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

        // Short human-readable name for the History panel (e.g. "Move", "Rename", "Delete").
        virtual std::string GetLabel() const
        {
            return "Edit";
        }
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
            PushCommand( std::make_unique<ByteCommand>( target, oldBytes, newBytes, size ) );
        }

        // Reflected-property STRING edit: before/after VALUES, restored by assignment. The value and the
        // object's representation are different things for a std::string, and only the value survives
        // being stored (see Push above).
        void PushString( std::string* target, std::string oldValue, std::string newValue )
        {
            PushCommand( std::make_unique<StringCommand>( target, std::move( oldValue ), std::move( newValue ) ) );
        }

        void PushCommand( std::unique_ptr<ICommand> command )
        {
            // A new edit invalidates the redo branch.
            m_Redo.clear();
            m_Undo.push_back( std::move( command ) );
            if ( m_Undo.size() > kMaxEntries )
                m_Undo.erase( m_Undo.begin() );
            ++m_Revision;
        }

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
            m_Undo.push_back( std::make_unique<JoinedCommand>( std::move( first ), std::move( second ) ) );
            ++m_Revision;
            return true;
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
                    m_Redo.push_back( std::move( cmd ) );
                    ++m_Revision;
                    return true;
                }
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
                    m_Undo.push_back( std::move( cmd ) );
                    ++m_Revision;
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

        // Drops only the pointer-based (volatile) entries; UUID-addressed structural commands survive.
        void DropVolatile()
        {
            auto drop = []( std::vector<std::unique_ptr<ICommand>>& stack )
            {
                std::erase_if( stack, []( const std::unique_ptr<ICommand>& c ) { return c->IsVolatile(); } );
            };
            drop( m_Undo );
            drop( m_Redo );
        }

        void Clear()
        {
            m_Undo.clear();
            m_Redo.clear();
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

            bool IsVolatile() const override
            {
                return m_First->IsVolatile() || m_Second->IsVolatile();
            }

            std::string GetLabel() const override
            {
                return m_First->GetLabel() + " + " + m_Second->GetLabel();
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
        uint64_t                               m_Revision = 0;
    };
} // namespace Desert::Editor

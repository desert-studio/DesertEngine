#pragma once


#include <Engine/Runtime/SelectionContext.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace Desert::Editor::Core
{
    // Editor-side selection state — now MULTI-selection: an ordered set of entity UUIDs, where the LAST
    // element is the PRIMARY selection (drives the Details panel and the gizmo pivot). Every mutation is
    // PUSHED into the engine-side Runtime::SelectionContext (the "highlight/outline these entities" hint),
    // so the engine never depends on the editor — the dependency stays editor -> engine.
    //
    // Single-selection call sites keep working unchanged: SetSelected() replaces the whole selection,
    // GetSelected() returns the primary.
    class SelectionManager final
    {
    public:
        // Replace the whole selection with this one entity (plain click).
        static void SetSelected( const Common::UUID& uuid )
        {
            m_Selection.assign( 1, uuid );
            Sync();
        }

        // The PRIMARY selection (most recently selected), or nullopt when nothing is selected.
        static const std::optional<Common::UUID>& GetSelected()
        {
            return m_Primary;
        }

        // The full ordered selection (last = primary).
        static const std::vector<Common::UUID>& GetSelection()
        {
            return m_Selection;
        }

        static size_t Count()
        {
            return m_Selection.size();
        }

        static bool IsSelected( const Common::UUID& uuid )
        {
            return std::find( m_Selection.begin(), m_Selection.end(), uuid ) != m_Selection.end();
        }

        // Appends (the entity becomes primary); re-selecting an already selected entity promotes it.
        static void AddToSelection( const Common::UUID& uuid )
        {
            std::erase( m_Selection, uuid );
            m_Selection.push_back( uuid );
            Sync();
        }

        static void RemoveFromSelection( const Common::UUID& uuid )
        {
            std::erase( m_Selection, uuid );
            Sync();
        }

        // Ctrl+click semantics.
        static void Toggle( const Common::UUID& uuid )
        {
            if ( IsSelected( uuid ) )
                RemoveFromSelection( uuid );
            else
                AddToSelection( uuid );
        }

        // Replace the selection wholesale (range select); the vector's last element becomes primary.
        static void SetSelection( std::vector<Common::UUID> uuids )
        {
            m_Selection = std::move( uuids );
            Sync();
        }

        static void ClearSelection()
        {
            m_Selection.clear();
            Sync();
        }

    private:
        static void Sync()
        {
            m_Primary = m_Selection.empty() ? std::nullopt
                                            : std::make_optional( m_Selection.back() );
            Runtime::SelectionContext::SetAll( m_Selection );
        }

        static inline std::vector<Common::UUID>   m_Selection;
        static inline std::optional<Common::UUID> m_Primary;
    };
} // namespace Desert::Editor::Core

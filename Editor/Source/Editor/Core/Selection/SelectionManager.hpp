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
        static void SetSelected( const Common::UUID& uuid );

        // The PRIMARY selection (most recently selected), or nullopt when nothing is selected.
        static const std::optional<Common::UUID>& GetSelected();

        // The full ordered selection (last = primary).
        static const std::vector<Common::UUID>& GetSelection();

        static size_t Count();

        static bool IsSelected( const Common::UUID& uuid );

        // Appends (the entity becomes primary); re-selecting an already selected entity promotes it.
        static void AddToSelection( const Common::UUID& uuid );

        static void RemoveFromSelection( const Common::UUID& uuid );

        // Ctrl+click semantics.
        static void Toggle( const Common::UUID& uuid )
        {
            if ( IsSelected( uuid ) )
                RemoveFromSelection( uuid );
            else
                AddToSelection( uuid );
        }

        // Replace the selection wholesale (range select); the vector's last element becomes primary.
        static void SetSelection( std::vector<Common::UUID> uuids );

        static void ClearSelection();

    private:
        static void Sync();

        static std::vector<Common::UUID> m_Selection;
        static std::optional<Common::UUID> m_Primary;
    };
} // namespace Desert::Editor::Core

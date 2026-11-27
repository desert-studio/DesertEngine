#include "SelectionManager.hpp"

namespace Desert::Editor::Core
{
    auto SelectionManager::Sync() -> void
    {
        m_Primary = m_Selection.empty() ? std::nullopt
                                        : std::make_optional( m_Selection.back() );
        Runtime::SelectionContext::SetAll( m_Selection );
    }

    auto SelectionManager::ClearSelection() -> void
    {
        m_Selection.clear();
        Sync();
    }

    auto SelectionManager::SetSelection( std::vector<Common::UUID> uuids ) -> void
    {
        m_Selection = std::move( uuids );
        Sync();
    }

    auto SelectionManager::RemoveFromSelection( const Common::UUID& uuid ) -> void
    {
        std::erase( m_Selection, uuid );
        Sync();
    }

    auto SelectionManager::AddToSelection( const Common::UUID& uuid ) -> void
    {
        std::erase( m_Selection, uuid );
        m_Selection.push_back( uuid );
        Sync();
    }

    auto SelectionManager::IsSelected( const Common::UUID& uuid ) -> bool
    {
        return std::find( m_Selection.begin(), m_Selection.end(), uuid ) != m_Selection.end();
    }

    auto SelectionManager::Count() -> size_t
    {
        return m_Selection.size();
    }

    auto SelectionManager::GetSelection() -> const std::vector<Common::UUID>&
    {
        return m_Selection;
    }

    auto SelectionManager::GetSelected() -> const std::optional<Common::UUID>&
    {
        return m_Primary;
    }

    auto SelectionManager::SetSelected( const Common::UUID& uuid ) -> void
    {
        m_Selection.assign( 1, uuid );
        Sync();
    }

    std::vector<Common::UUID> SelectionManager::m_Selection;

    std::optional<Common::UUID> SelectionManager::m_Primary;
}

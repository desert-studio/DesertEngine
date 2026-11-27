#include <Engine/Runtime/SelectionContext.hpp>

namespace Desert::Runtime
{
    auto SelectionContext::Changed() -> void
    {
        s_SelectedSet.clear();
        s_SelectedSet.insert( s_Selected.begin(), s_Selected.end() );
        ++s_Revision;
    }

    auto SelectionContext::IsBindPosePreview( const Common::UUID& uuid ) -> bool
    {
        return s_BindPosePreview.has_value() && *s_BindPosePreview == uuid;
    }

    auto SelectionContext::SetBindPosePreview( const std::optional<Common::UUID>& uuid ) -> void
    {
        s_BindPosePreview = uuid;
    }

    auto SelectionContext::GetRevision() -> uint64_t
    {
        return s_Revision;
    }

    auto SelectionContext::Clear() -> void
    {
        s_Selected.clear();
        Changed();
    }

    auto SelectionContext::Contains( const Common::UUID& uuid ) -> bool
    {
        return s_SelectedSet.contains( uuid );
    }

    auto SelectionContext::GetAll() -> const std::vector<Common::UUID>&
    {
        return s_Selected;
    }

    auto SelectionContext::Get() -> std::optional<Common::UUID>
    {
        if ( s_Selected.empty() )
            return std::nullopt;
        return s_Selected.back();
    }

    auto SelectionContext::SetAll( std::vector<Common::UUID> uuids ) -> void
    {
        s_Selected = std::move( uuids );
        Changed();
    }

    auto SelectionContext::Set( const Common::UUID& uuid ) -> void
    {
        s_Selected.assign( 1, uuid );
        Changed();
    }

    std::vector<Common::UUID> SelectionContext::s_Selected;

    std::unordered_set<Common::UUID> SelectionContext::s_SelectedSet;

    uint64_t SelectionContext::s_Revision = 0;

    std::optional<Common::UUID> SelectionContext::s_BindPosePreview;
}

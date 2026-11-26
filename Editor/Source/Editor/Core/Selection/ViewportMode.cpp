#include "ViewportMode.hpp"

namespace Desert::Editor::Core
{
    auto ViewportMode::IsModeling() -> bool
    {
        return s_Mode == EditorMode::Modeling;
    }

    auto ViewportMode::IsFoliage() -> bool
    { return s_Mode == EditorMode::Foliage; }

    auto ViewportMode::SetChangeListener( ChangeListener listener ) -> void
    { s_OnChange = listener; }

    auto ViewportMode::Set( EditorMode mode ) -> void
    {
        if ( s_Mode == mode )
            return;
        s_Mode = mode;
        if ( s_OnChange != nullptr )
            s_OnChange( mode );
    }

    auto ViewportMode::Get() -> EditorMode
    { return s_Mode; }

    EditorMode ViewportMode::s_Mode = EditorMode::Select;

    ViewportMode::ChangeListener ViewportMode::s_OnChange = nullptr;
}

#include "Binding.hpp"

namespace Desert::Animation::Timeline
{
    const char* ToString( const BindingKind kind )
    {
        switch ( kind )
        {
            case BindingKind::Sequence:
                return "Sequence";
            case BindingKind::Bone:
                return "Bone";
            case BindingKind::Entity:
                return "Entity";
            case BindingKind::Widget:
                return "Widget";
        }
        // A kind outside the enum is refused where it is READ; this names it for the log that says so.
        return "Unknown";
    }
} // namespace Desert::Animation::Timeline

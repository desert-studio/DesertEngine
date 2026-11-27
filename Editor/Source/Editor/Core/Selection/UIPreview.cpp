#include "UIPreview.hpp"

namespace Desert::Editor::Core
{
    auto UIPreview::Get() -> UIPreview&
    {
        static UIPreview s;
        return s;
    }
}

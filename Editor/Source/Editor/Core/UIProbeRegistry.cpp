#include "UIProbeRegistry.hpp"

namespace Desert::Editor::Core
{
    auto UIProbeRegistry::Get() -> UIProbeRegistry&
    {
        static UIProbeRegistry s;
        return s;
    }
}

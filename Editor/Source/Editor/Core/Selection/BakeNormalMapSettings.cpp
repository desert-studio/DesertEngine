#include "BakeNormalMapSettings.hpp"

namespace Desert::Editor::Core
{
    BakeNormalMapArgs& BakeNormalMapSettings()
    {
        static BakeNormalMapArgs s_Args;
        return s_Args;
    }
} // namespace Desert::Editor::Core

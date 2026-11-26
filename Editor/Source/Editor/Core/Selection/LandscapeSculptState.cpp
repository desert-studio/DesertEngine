#include "LandscapeSculptState.hpp"

namespace Desert::Editor::Core
{
    auto LandscapeSculptState::Get() -> LandscapeSculptState&
    {
        static LandscapeSculptState s_State;
        return s_State;
    }
}

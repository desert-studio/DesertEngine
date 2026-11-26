#include "ControlNudgeRequest.hpp"

namespace Desert::Editor::Core
{
    auto ControlNudgeRequests::Get() -> State&
    {
        static State s_State;
        return s_State;
    }
}

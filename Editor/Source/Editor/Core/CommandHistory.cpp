#include "CommandHistory.hpp"

namespace Desert::Editor
{
    auto CommandHistory::Get() -> CommandHistory&
    {
        static CommandHistory s_Instance;
        return s_Instance;
    }
}

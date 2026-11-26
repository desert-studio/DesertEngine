#include "ModelingState.hpp"

namespace Desert::Editor::Core
{
    auto ModelingState::Get() -> ModelingState&
    {
        static ModelingState s;
        return s;
    }
}

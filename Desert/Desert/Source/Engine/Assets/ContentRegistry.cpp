#include <Engine/Assets/ContentRegistry.hpp>

namespace Desert::Assets::ContentRegistry::Detail
{
    State& Get_()
    {
        static State state;
        return state;
    }
}

#include "ShotOptions.hpp"

namespace Desert::Editor
{
    auto ShotOptions::Get() -> ShotOptions&
    {
        static ShotOptions s;
        return s;
    }
}

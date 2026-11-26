#include "ControlChannelOptions.hpp"

namespace Desert::Editor::Control
{
    auto ControlChannelOptions::Get() -> ControlChannelOptions&
    {
        static ControlChannelOptions s;
        return s;
    }
}

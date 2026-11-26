#include "InputInjection.hpp"

namespace Desert::Editor::Control
{
    auto InputInjection::Queue() -> std::deque<InputFrame>&
    {
        static std::deque<InputFrame> queue;
        return queue;
    }
}

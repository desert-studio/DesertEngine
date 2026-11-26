#include "PointerDrag.hpp"

namespace Desert::Editor::Control
{
    auto PointerInjection::Targets() noexcept -> std::array<PointerTargetRect, kSubjectCount>&
    {
        static std::array<PointerTargetRect, kSubjectCount> targets{};
        return targets;
    }

    auto PointerInjection::State() noexcept -> Playback&
    {
        static Playback state;
        return state;
    }
}

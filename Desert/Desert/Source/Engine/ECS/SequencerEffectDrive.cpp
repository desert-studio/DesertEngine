#include <Engine/ECS/SequencerEffectDrive.hpp>

namespace Desert::ECS
{
    SequencerEffectHookTable& SequencerEffectHooks()
    {
        static SequencerEffectHookTable hooks;
        return hooks;
    }
}

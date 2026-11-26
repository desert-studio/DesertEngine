#pragma once

// AN ENTITY THE RUNNING CODE OWNS (UE RF_Transient on an actor a subsystem spawned): made by a module while the
// scene ticks and made again by it whenever it is missing - the effects a river's fall plays at its foot
// (Water/WaterFallEffects). The scene's documents do not hold it: SceneSerializer leaves it out of every one (save,
// the Play snapshot, LiveEntities - hence the packages and the undo history), and the Outliner does not list it.
// Never reflected, never written.

namespace Desert::ECS
{
    struct TransientComponent
    {
    };
} // namespace Desert::ECS

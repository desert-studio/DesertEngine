#pragma once

namespace Desert::ECS
{
    // A REGISTRY CONTEXT VALUE, written by the editor's viewport each frame: while its foliage mode is active the
    // scene draws every instance, so the brush shows and edits what the file holds. Absent in a game.
    struct FoliageEditing
    {
        bool Active = false;
    };
} // namespace Desert::ECS

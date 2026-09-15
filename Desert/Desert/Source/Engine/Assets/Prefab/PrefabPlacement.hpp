#pragma once

// WHERE A PREFAB MAY BE PUT — the one question a UI prefab asks that a world prefab does not.
//
// A UI element is not drawn because it exists; it is drawn because a UICanvas walk reaches it
// (UICanvasLayout::EnumerateCanvas starts AT a canvas and descends). So a prefab whose root is a
// UILayout, instantiated at the scene root the way every prefab was instantiated before Ю19, produces
// entities that are present in the outliner, selectable, correctly serialized — and cover zero pixels,
// with nothing logged. That is the shape §1.4 of the contract calls an empty successful answer, and it
// is the whole of what "UI prefab" needed that "prefab" did not.
//
// THE RULE IS PURE AND LIVES HERE, not inside the instantiator, because both the instantiator and the
// editor menus need the same verdict: a menu that offers an action the engine will refuse is the two-
// places-that-must-agree shape this project keeps paying for. Component keys are read off the parsed
// record rather than off a live entity, so the answer is available before anything is created.

#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <string>
#include <string_view>

namespace Desert::Assets
{
    // What a prefab's first record is. Derived from the component keys the record carries, which is the
    // same registry key ComponentRegistry writes — the classification cannot drift from the file.
    enum class PrefabRootKind
    {
        World,     // no UI components: a mesh, a light, a rig. Draws wherever it is.
        UIElement, // a UILayout and no canvas of its own: needs a canvas ABOVE it to be drawn at all.
        UICanvas   // carries its own canvas: it is a UI tree's root and must not be put inside another.
    };

    [[nodiscard]] PrefabRootKind ClassifyPrefabRoot( const EntityData& rootRecord );

    struct PrefabPlacementVerdict
    {
        bool        Allowed = false;
        std::string Refusal; // empty when Allowed
    };

    // May a prefab of @p kind be instantiated under a target that is (or is not) inside a canvas?
    //
    // @p targetUnderCanvas is true when the entity the instance will hang under carries a
    // UICanvasComponent itself or has one among its ancestors — UI::CanvasOf's question, asked by the
    // caller so this stays free of ECS.
    // @p targetName is what the refusal calls the target ("the scene root" when there is none).
    [[nodiscard]] PrefabPlacementVerdict CheckPrefabPlacement( PrefabRootKind kind, bool targetUnderCanvas,
                                                               std::string_view source,
                                                               std::string_view targetName );
} // namespace Desert::Assets

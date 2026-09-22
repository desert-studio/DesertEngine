#pragma once

#include <Engine/Desert.hpp>

#include <glm/glm.hpp>

namespace Desert::Editor::Tools
{
    // ── WHY PICKING ANSWERS WITH A NAME AND NOT WITH NOTHING ─────────────────────────────────────────
    //
    // Pick() used to return void, and every one of its early exits was a bare `return`. That was
    // survivable while all of them meant the same thing ("no camera / the gizmo has the cursor"), but a
    // LOCK adds an exit that a user is actively waiting on: they clicked a thing, it was refused, and
    // "nothing happened" is indistinguishable from a missed ray or a dead mouse button.
    //
    // So the outcomes are enumerated. The caller can say WHY the click did nothing; a test can assert
    // that a locked entity produces RefusedLocked and not Missed, which is the difference between a lock
    // that works and a raycast that happens to be broken.
    enum class PickOutcome
    {
        NoCamera,            // the scene has no camera to cast through — nothing was even attempted
        RefusedGizmoHovered, // the cursor is over a gizmo handle; the drag owns this click
        RefusedLocked,       // the ray HIT, and the entity it hit is locked for authoring
        Selected,            // the hit entity replaced the selection
        Toggled,             // Ctrl held: the hit entity was added to / removed from the selection
        Missed,              // the ray hit nothing, and the selection was left alone (Ctrl held)
        Cleared,             // the ray hit nothing, so the selection was cleared (click empty space)
    };

    // Human-readable form, for logs and test failure messages. Every enumerator has one; the switch in
    // the .cpp has no default, so adding an outcome without naming it fails to compile.
    const char* Describe( PickOutcome outcome );

    // Object picking, extracted from ViewportPanel (god-object split). Casts the cursor ray against scene
    // meshes (engine-owned Scene::Raycast) and selects the hit entity (resolving to the prefab root) via
    // SelectionManager. No-op while the gizmo is hovered/dragged. The host gates mode / viewport-hover.
    class PickingController
    {
    public:
        // additive = Ctrl held: toggles the hit entity in the multi-selection instead of replacing it.
        // A miss with additive=false clears the selection (click empty space to deselect).
        //
        // NO_DISCARD because the refusals are the interesting half: a caller that throws the answer away
        // is a caller that cannot tell a locked entity from an empty sky.
        // @p camera is THIS VIEWPORT's camera — the ray is cast from the angle the user clicked in, not
        // from view 0's. Asking the scene picked the wrong entity from a second viewport.
        [[nodiscard]] PickOutcome Pick( ::Desert::Core::Scene&                         scene,
                                        const std::shared_ptr<::Desert::Core::Camera>& camera,
                                        const glm::vec2& mouseViewport, const glm::vec2& viewportSize,
                                        bool gizmoHovered, bool additive = false );
    };
} // namespace Desert::Editor::Tools

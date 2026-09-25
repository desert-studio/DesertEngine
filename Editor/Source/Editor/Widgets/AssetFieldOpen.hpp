#pragma once

#include <cstdint>

namespace Desert::Editor
{
    // "OPEN" AND "SHOW IN BROWSER" ON AN ASSET FIELD — UE's SPropertyEditorAsset gesture: double-click the
    // field, or right-click it for a menu that also offers "Browse to asset". Acts on the LAST ImGui item, so
    // a caller that draws its field as several widgets wraps them in BeginGroup/EndGroup and calls this once
    // after the group; one call per field, not one per branch that draws a field differently.
    //
    // Both actions are queued as Core::AssetFieldRequests and answered by EditorLayer through
    // Core::RequestOpenAsset and the `Browse` folder navigation. A type that nothing opens yet (a mesh, a
    // font) keeps the menu item: the refusal is logged by name and number, which is the truth about the
    // editor, where a hidden item would only look like a missing feature.
    //
    // The popup id lives in the caller's ID scope, so two fields on one row need the PushID the row already
    // has for its own widgets. `handle == 0` (an empty slot) draws nothing: there is nothing to open.
    void DrawAssetFieldOpen( uint64_t handle );
} // namespace Desert::Editor

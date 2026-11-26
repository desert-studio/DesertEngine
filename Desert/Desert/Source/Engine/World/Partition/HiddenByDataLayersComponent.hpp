#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // HIDDEN IN THE EDITOR BY ITS DATA LAYERS - UE AActor::bHiddenEdLayer (WP-DATA-LAYER-OUTLINER). A transient
    // tag: no COMPONENT key, so it is never saved, copied, pasted or snapshotted for undo or Play, and never
    // authored. The editor's data-layer state stamps it on an entity none of whose data layers is visible and
    // lifts it again (Editor DataLayerEditor::UpdateEntityVisibility; never while the scene plays). It is NOT
    // VisibilityComponent: that bit is the author's and is saved; this one is one user's view of the world in one
    // session. ECS::IsHidden honours both, so every collector and the viewport's picking do; Core::Scene marks
    // render state dirty on it.
    struct HiddenByDataLayersComponent
    {
    };
} // namespace Desert::ECS

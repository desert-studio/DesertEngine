#pragma once

#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>

#include <vector>

namespace Desert::Graphic::System
{
    // THE SUN'S VIRTUAL SHADOW MAP, before its page passes (SceneRenderer::AddFrameVirtualShadows): what
    // changed among the casters since the last frame the map was drawn. A static, generic or still instanced
    // caster and every non-mesh caster (IShadowCaster::AppendVirtualShadowEntries) that was added, removed or
    // changed hands its old and its new box; a caster that moves by itself (skinned, swaying foliage) hands
    // its box now and last frame, every frame. StaticCastersChanged: a masked caster's pipeline was still
    // compiling when the pages were drawn, so the cached pages lack it and every one is dropped.
    // Its own header (UE: FVirtualShadowMapArrayCacheManager's invalidation lists live apart from the mesh
    // passes): SceneRenderer keeps one across frames without the whole MeshRenderer.hpp.
    struct VirtualShadowInvalidation
    {
        std::vector<VsmInvalidationBox> Boxes;        // the still casters that changed: both layers
        std::vector<VsmInvalidationBox> MovableBoxes; // every mover instance, now and last frame: merged layer
        bool                            StaticCastersChanged = false;
    };
} // namespace Desert::Graphic::System

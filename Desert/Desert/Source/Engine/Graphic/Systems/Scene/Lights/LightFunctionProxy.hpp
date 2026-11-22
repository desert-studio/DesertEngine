#pragma once

// The light-function value a light command carries, apart from the atlas pass (LightFunctionAtlas.hpp), so the
// ECS light systems and their commands do not reach the pass's pipeline and render-graph headers.
#include <Engine/Assets/Common.hpp>

#include <glm/vec3.hpp>

namespace Desert::Graphic::System
{
    // One light's light function as the ECS hands it to the renderer (UE ULightComponent::LightFunctionMaterial /
    // LightFunctionScale; LIGHT-FUNCTIONS). Per frame, copied (a command replayed into several views hands each
    // the same value). A null Material = no light function: the light keeps LightFunctionTile -1 and shades with a
    // factor of exactly 1 (Mesh/LightFunctionAtlas.glslh). The light's frame (position, forward, tangent, cone) is
    // the payload's own (the IES frame), so the proxy carries only what the payload does not.
    struct LightFunctionProxy
    {
        Assets::AssetHandle Material = Assets::AssetHandle::Null(); // a `Domain LightFunction` material
        glm::vec3           Scale{ 1024.0f, 1024.0f, 1024.0f };     // cm, UE default 1024
    };
} // namespace Desert::Graphic::System

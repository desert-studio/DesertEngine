#pragma once

#include <entt/entt.hpp>

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

#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/Materials/Material.hpp>

namespace Desert::ECS
{
    // HOW A LANDSCAPE LOOKS (UE: ALandscape::LandscapeMaterial), on the root entity beside its
    // LandscapeComponent. Apart from the frame because the frame is authored as raw numbers
    // (MakeAuthored) and this is reflected: one asset handle the Details panel builds.
    struct LandscapeMaterialData
    {
        REFLECT()

        // The landscape's material, a `.demat` of domain Terrain like every other material — the surface
        // is drawn by ONE program whose parameters (Tint) this material sets. The ground's layers are the
        // root's painted layer infos (LandscapeComponent::Layers), never built-in ones: where nothing is
        // painted the first layer shows, as UE's landscape does. One handle and not a slot vector: a vector
        // would promise a material per layer, and nothing downstream could consume one. Unset = the
        // shader's own schema defaults.
        //
        // Read by Engine/ECS/System/LandscapeECSSystem.cpp, which resolves it through
        // Runtime::MaterialService and forwards the values as named overrides on every tile of the root.
        //
        // Hidden from the auto-built Details: the builder's asset slot is texture-oriented, and the
        // Landscape Material entry draws a material field with an Edit button that opens the Material
        // Editor window. Still serialized; Hidden is editor-only.
        PROPERTY( DisplayName( "Material" ), Category( "Landscape" ), Asset<MaterialAsset>, Hidden )
        Assets::AssetHandle Material;
    };

    struct LandscapeMaterialComponent
    {
        COMPONENT( Key( "LandscapeMaterial" ), Block( Data ), Run( Landscape ) )
        LandscapeMaterialData Data;
    };
} // namespace Desert::ECS

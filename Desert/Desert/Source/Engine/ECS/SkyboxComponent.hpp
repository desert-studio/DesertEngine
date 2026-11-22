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

namespace Desert::ECS
{
    // The HDR-cubemap background, and nothing else. The procedural atmosphere (palette, sun, stars, the
    // IBL bake request) moved to SkyAtmosphereComponent; the old fields are gone rather than deprecated,
    // so there is exactly one place each value can live.
    //
    // Reflected (REFLECT/PROPERTY) so it (de)serializes generically — the SkyboxHandle round-trips as an
    // asset PATH via the serializer's AssetResolver. Fields kept flat (no Data sub-struct) so existing
    // accessors are unchanged.
    struct SkyboxComponent
    {
        COMPONENT( Key( "Skybox" ), Whole, Run( SkyAndAtmosphere ) )
        REFLECT()

        // Hidden from the auto-generated Details (the widget draws a proper SkyboxAsset picker + DnD instead
        // of the builder's texture-oriented asset slot). Still serialized — Hidden is editor-only.
        PROPERTY( DisplayName( "Skybox" ), Category( "Skybox" ), Asset<SkyboxAsset>, Hidden )
        Assets::AssetHandle SkyboxHandle;

        // ── THE AUTHORED LOOK ─────────────────────────────────────────────────────────────────────────
        //
        // All three reach the frame through ONE route: ECS::SkyLookOf packs them into a Graphic::SkyLook,
        // and that one value is applied wherever the environment cubes are SAMPLED — the backdrop, and
        // every lit surface's ambient and reflections (Shaders/Common/SkyLook.glslh). Intensity used to be
        // applied to the sky pass alone and therefore lit nothing; the shader census in
        // Tests/Engine/SkyPanorama is what keeps that state unreachable now.
        //
        // THE PRICE IS A UNIFORM WRITE PER FRAME. They used to be baked into the cubes, and every slider
        // value cost a device-idling rebake of ~0.7 s; the cubes are now the file as authored.

        PROPERTY( DisplayName( "Intensity" ), Category( "Skybox" ), Range( 0.0f, 10.0f ) )
        float Intensity = 1.0f;

        // Degrees about the world's up axis. Answers the one thing an author cannot do to a panorama
        // from outside: line the sun that is baked into the image up with the scene's directional light.
        PROPERTY( DisplayName( "Rotation" ), Category( "Skybox" ), Range( 0.0f, 360.0f ) )
        float Rotation = 0.0f;

        // Linear grade over the whole sky. White is the file as authored.
        PROPERTY( DisplayName( "Tint" ), Category( "Skybox" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );
    };
} // namespace Desert::ECS

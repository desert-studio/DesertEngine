#pragma once

#include <glm/glm.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

// A REFLECTION CAPTURE (UE: USphereReflectionCaptureComponent / UBoxReflectionCaptureComponent). The scene is
// rendered from the entity's position into one cube, the cube is GGX-prefiltered across its mips and stored in
// the renderer's capture cube array (Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp). A lit pixel
// inside the capture's influence reads that cube PARALLAX-CORRECTED (the reflection ray is intersected with the
// capture's sphere or box and the cube is looked up towards the hit, not along the bare ray), so a glossy floor
// reflects the room it stands in rather than the sky. Where no capture covers a pixel the sky's prefiltered cube
// is read, as before; both together are the reflection fallback every reflection method lands on when its own
// trace misses (SSR today).
//
// The shape is one component with a Shape field rather than UE's two actor classes: both shapes share every
// other field and the whole capture path, and a box is a sphere's influence test swapped for a box one.
namespace Desert::ECS
{
    enum class ReflectionCaptureShape
    {
        Sphere,
        Box
    };

    struct ReflectionCaptureData
    {
        REFLECT()

        PROPERTY( DisplayName( "Shape" ), Category( "Reflection Capture" ), Summary,
                  Tooltip( "Sphere: the influence and the parallax proxy are a sphere of Influence Radius. Box: "
                           "both are the box of Box Extent around the entity (rotated with it) - the shape for a "
                           "room, whose walls the parallax then follows." ) )
        ReflectionCaptureShape Shape = ReflectionCaptureShape::Sphere;

        PROPERTY( DisplayName( "Influence Radius" ), Category( "Reflection Capture" ), Length,
                  Range( 1.0f, 100000.0f ), Summary,
                  Tooltip( "Sphere shape: radius around the entity inside which surfaces read this capture, and "
                           "the sphere the reflection ray is projected onto. UE's default, 3000 cm." ) )
        float InfluenceRadius = 3000.0f;

        PROPERTY( DisplayName( "Box Extent" ), Category( "Reflection Capture" ), Length,
                  Tooltip( "Box shape: half-size of the box, around the entity and in its rotation, inside which "
                           "surfaces read this capture and onto whose walls the reflection is projected." ) )
        glm::vec3 BoxExtent = glm::vec3( 1000.0f, 400.0f, 1000.0f );

        PROPERTY( DisplayName( "Box Transition Distance" ), Category( "Reflection Capture" ), Length,
                  Range( 0.0f, 10000.0f ),
                  Tooltip( "Box shape: distance inside the box walls across which the capture fades in, so a "
                           "neighbouring capture or the sky takes over without a seam. UE's default, 100 cm." ) )
        float BoxTransitionDistance = 100.0f;

        PROPERTY( DisplayName( "Capture Offset" ), Category( "Reflection Capture" ), Length, Advanced,
                  Tooltip( "Where the cube is captured from, relative to the entity - move it out of a table or "
                           "a wall without moving the influence." ) )
        glm::vec3 CaptureOffset = glm::vec3( 0.0f );

        PROPERTY( DisplayName( "Brightness" ), Category( "Reflection Capture" ), Range( 0.0f, 10.0f ),
                  Units( "x" ), Tooltip( "Multiplier on the captured radiance (UE: Brightness)." ) )
        float Brightness = 1.0f;
    };

    struct ReflectionCaptureComponent
    {
        COMPONENT( Key( "ReflectionCapture" ), Block( Data ), Run( SkyAndAtmosphere ) )
        ReflectionCaptureData Data;
    };
} // namespace Desert::ECS

#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <cstdint>

namespace Desert::ECS
{
    // The two shapes a local fog volume is voxelised as (Graphic::LocalFogVolumeGpu Albedo.w, the froxel
    // shader's VolumetricFogLocalVolumeExtinction): the unit sphere or the [-1, 1] cube, scaled by Extent and
    // placed by the entity's world transform.
    enum class LocalFogVolumeShape : uint8_t
    {
        Sphere,
        Box
    };

    // UE 5.3's Local Fog Volume (ULocalFogVolumeComponent), voxelised into the volumetric fog's froxel grid
    // (VolumetricFogRenderer) rather than drawn by a pass of its own: inside the grid it is the same lit medium
    // as the height fog - shadowed by the sun's cascades and the cloud layer, lit by the local lights - so a fog
    // bank in a barn and the height fog outside it can never disagree about the light.
    //
    // THE PLACEMENT IS NOT A FIELD. The volume's centre, rotation and scale are the entity's world transform;
    // Extent is the half size (cm) of the shape in the entity's local space, so a scaled entity scales its fog.
    //
    // UNITS. Extent in world units (centimetres, Length). The extinctions are UE's authored "per metre"
    // coefficients and are converted to per centimetre once, in Graphic::PackLocalFogVolume. The height falloff
    // and offset live in the shape's unit space (-1 at the bottom, +1 at the top), as UE's do.
    //
    // ONLY WHERE THE FROXELS RUN: the volume needs the scene's Exponential Height Fog with Volumetric Fog on (UE's
    // rule for volumetric local fog); its own density then lights the grid even when the height fog has none.
    struct LocalFogVolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Local Fog Volume" ), Summary,
                  Tooltip( "Off: the volume is not gathered; with no enabled volume in the scene the froxels "
                           "loop over none and the frame is the height fog's alone." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Shape" ), Category( "Local Fog Volume" ), Summary,
                  Tooltip( "Sphere (UE's local fog volume) or Box (a room, a barn, a cave mouth)." ) )
        LocalFogVolumeShape Shape = LocalFogVolumeShape::Sphere;

        PROPERTY( DisplayName( "Extent" ), Category( "Local Fog Volume" ), Length, Range( 1.0f, 1000000.0f ),
                  Tooltip( "Half size of the shape along the entity's local X, Y, Z (cm): the sphere's radii "
                           "or the box's half extents, before the entity's scale." ) )
        glm::vec3 Extent = { 500.0f, 500.0f, 500.0f };

        PROPERTY( DisplayName( "Radial Fog Extinction" ), Category( "Local Fog Volume" ), Range( 0.0f, 10.0f ),
                  Tooltip( "Extinction at the centre (per metre), falling linearly to zero at the shape's edge "
                           "(UE RadialFogExtinction)." ) )
        float RadialFogExtinction = 1.0f;

        PROPERTY( DisplayName( "Height Fog Extinction" ), Category( "Local Fog Volume" ), Range( 0.0f, 10.0f ),
                  Tooltip( "Extinction at the height offset (per metre), thinning exponentially upward "
                           "(UE HeightFogExtinction)." ) )
        float HeightFogExtinction = 1.0f;

        PROPERTY(
             DisplayName( "Height Fog Falloff" ), Category( "Local Fog Volume" ), Range( 0.0f, 50.0f ),
             Tooltip( "How fast the height fog thins, per unit of the shape's half height: 0 is uniform, "
                      "2 leaves e^-2 of the density at the top when the offset is 0 (UE HeightFogFalloff)." ) )
        float HeightFogFalloff = 2.0f;

        PROPERTY( DisplayName( "Height Fog Offset" ), Category( "Local Fog Volume" ), Range( -1.0f, 1.0f ),
                  Tooltip( "Where in the shape's height the height fog has its full extinction: -1 the bottom, "
                           "0 the centre, 1 the top (UE HeightFogOffset)." ) )
        float HeightFogOffset = 0.0f;

        PROPERTY( DisplayName( "Edge Fade" ), Category( "Local Fog Volume" ), Range( 0.001f, 1.0f ),
                  Tooltip( "Width of the fade at the shape's boundary, as a fraction of its half size, so the "
                           "volume never shows its edge as a line." ) )
        float EdgeFade = 0.1f;

        PROPERTY( DisplayName( "Albedo" ), Category( "Local Fog Volume" ), Color,
                  Tooltip( "Fraction of the extinguished light that is scattered rather than absorbed, per "
                           "channel (UE FogAlbedo)." ) )
        glm::vec3 Albedo = { 1.0f, 1.0f, 1.0f };

        PROPERTY( DisplayName( "Emissive" ), Category( "Local Fog Volume" ), Color,
                  Tooltip( "Light the medium emits on its own, per unit of its extinction (UE FogEmissive)." ) )
        glm::vec3 Emissive = { 0.0f, 0.0f, 0.0f };
    };

    struct LocalFogVolumeComponent
    {
        COMPONENT( Key( "LocalFogVolume" ), Block( Data ), Run( SkyAndAtmosphere ) )
        LocalFogVolumeData Data;
    };
} // namespace Desert::ECS

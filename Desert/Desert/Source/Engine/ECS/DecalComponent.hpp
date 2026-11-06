#pragma once

// A DEFERRED DECAL (DECALS-DBUFFER; UE UDecalComponent / ADecalActor -> FDeferredDecalProxy). A box projector: the
// entity's transform places and orients the box, Extent is its half size, and every opaque surface inside it
// receives the Material (a `Domain DeferredDecal` shader) through the view's DBuffer (Common/DBuffer.glslh):
// "Deferred: Decals" (System::DecalRenderer) draws each box over the depth prepass into DBufferA/B/C before the
// G-buffer, and each G-buffer writer folds the DBuffer into its own surface. x / y of the box are the texture
// plane, z the projection axis (UE projects along X; the box is the same, only the axis name differs).
// Units: centimetres.

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Assets/Common.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::ECS
{
    struct DecalData
    {
        REFLECT()

        // The decal's material: a `Domain DeferredDecal` shader (ShaderProgramMeta.hpp kDecalPathDomain). Empty =
        // the engine's default decal shader (Programs/Decals/DefaultDecal.shader).
        PROPERTY( DisplayName( "Material" ), Category( "Decal" ), Summary, Asset<MaterialAsset>,
                  Tooltip( "UE DecalMaterial: a material in the Deferred Decal domain. Empty draws the engine's "
                           "default decal." ) )
        Assets::AssetHandle Material;

        PROPERTY( DisplayName( "Extent" ), Category( "Decal" ), Summary, Length, Range( 0.1f, 1.0e6f ),
                  Tooltip( "UE DecalSize / 2: the box's half size along its x / y (the texture plane) and z (the "
                           "projection axis), before the entity's scale." ) )
        glm::vec3 Extent = { 128.0f, 128.0f, 128.0f };

        PROPERTY( DisplayName( "Sort Order" ), Category( "Decal" ), Range( -1000, 1000 ),
                  Tooltip( "UE SortOrder: a higher value draws later, over the decals below it. Equal values draw "
                           "in entity-id order, so the stack never flickers." ) )
        int32_t SortOrder = 0;

        PROPERTY( DisplayName( "Channels" ), Category( "Decal" ), Range( 0, 7 ),
                  Tooltip( "Which DBuffer channels this decal may write (bits: 1 base colour, 2 normal, 4 "
                           "roughness / metallic / AO), masked with what the material writes. 7 = all." ) )
        uint32_t ChannelMask = 7u;

        PROPERTY( DisplayName( "Fade Start" ), Category( "Fade" ), Length, Range( 0.0f, 1.0e7f ),
                  Tooltip( "UE FadeStartDelay's distance twin: the camera distance at which the decal starts to "
                           "fade out. 0 with Fade Length 0 = never fades." ) )
        float FadeStart = 0.0f;

        PROPERTY( DisplayName( "Fade Length" ), Category( "Fade" ), Length, Range( 0.0f, 1.0e7f ),
                  Tooltip( "The distance over which the decal fades from full to nothing past Fade Start; 0 = no "
                           "distance fade." ) )
        float FadeLength = 0.0f;

        PROPERTY( DisplayName( "Fade Screen Size" ), Category( "Fade" ), Range( 0.0f, 1.0f ),
                  Tooltip( "UE FadeScreenSize: the fraction of the screen height below which the box is not "
                           "drawn. 0 = always drawn." ) )
        float FadeScreenSize = 0.01f;

        PROPERTY( DisplayName( "Edge Fade" ), Category( "Fade" ), Range( 0.0f, 1.0f ),
                  Tooltip( "The share of the projection axis, at each end of the box, over which the decal fades "
                           "to nothing (Common/DBuffer.glslh DesertDecalBoxFade)." ) )
        float EdgeFade = 0.1f;
    };

    struct DecalComponent
    {
        COMPONENT( Key( "Decal" ), Block( Data ), Run( ActorsAndUI ) )
        DecalData Data;
    };
} // namespace Desert::ECS

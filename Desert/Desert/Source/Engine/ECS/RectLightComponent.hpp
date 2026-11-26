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

#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/LightUnits.hpp>

namespace Desert::ECS
{
    // Rect light (UE URectLightComponent): a one-sided rectangular emitter at the entity's position, its face
    // aimed along the entity's forward (-Z of the world rotation), Source Width along the entity's X. Lit as an
    // area - the exact Lambert irradiance of the rect the barn doors leave visible, the specular aimed at the
    // rect itself (Mesh/LightSources.glslh DesertRectLightAt, Mesh/RectLight.glslh) - and culled, shadowed (a
    // cube from its centre) and sampled as a local light, as UE does (RectLightSystem: it rides the point list).
    struct RectLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE URectLightComponent Intensity / IntensityUnits: the candelas along the rect's normal (default 8 cd,
        // as the other local lights). Lumens are the whole flux of a Lambertian emitter, pi x the candelas. The
        // renderer reads it only through LightPhotometry::RectShaderLuminance.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 100000.0f ), Summary,
                  Tooltip( "Brightness in Intensity Units. Candelas: the intensity along the rect's normal; "
                           "Lumens: the total flux (pi x the candelas); EV100: 2^(EV-3) cd; Unitless: read as "
                           "candelas. Spread over the rect's area, so a larger rect is as bright, but softer." ) )
        float Intensity = 8.0f;

        PROPERTY( DisplayName( "Intensity Units" ), Category( "Light" ),
                  Tooltip( "What Intensity is measured in (UE Intensity Units). Changing it converts the value "
                           "so the light keeps its brightness." ) )
        LightUnits IntensityUnits = LightUnits::Candelas;

        // UE ULocalLightComponent::AttenuationRadius: the light ends here (its culling sphere and shadow range).
        PROPERTY( DisplayName( "Attenuation Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length,
                  Summary )
        float Range = 1000.0f;

        PROPERTY( DisplayName( "Source Width" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary,
                  Tooltip( "Width of the emitting rect, along the entity's X (UE SourceWidth)." ) )
        float SourceWidth = 64.0f;

        PROPERTY( DisplayName( "Source Height" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary,
                  Tooltip( "Height of the emitting rect, along the entity's Y (UE SourceHeight)." ) )
        float SourceHeight = 64.0f;

        PROPERTY( DisplayName( "Barn Door Angle" ), Category( "Light" ), Range( 0.0f, 90.0f ), Units( "deg" ),
                  Tooltip( "Angle of the barn doors from the rect's normal (UE BarnDoorAngle): 90 = folded flat, "
                           "no effect; smaller narrows the light toward a beam." ) )
        float BarnDoorAngle = 88.0f;

        PROPERTY( DisplayName( "Barn Door Length" ), Category( "Light" ), Range( 0.0f, 1000.0f ), Length,
                  Tooltip( "Length of the barn doors on all four edges of the rect (UE BarnDoorLength); "
                           "0 = none." ) )
        float BarnDoorLength = 20.0f;

        // THE RECT LIGHT'S SHADOW: the point light's cube (UE renders a rect light's whole-scene shadow as a point
        // light's), from the rect's centre. The biases are in shadow-map texels at the caster, as the point's.
        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Shadows" ) )
        bool CastShadows = true;

        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ), Units( "texels" ),
                  Tooltip( "How far every caster's depth is pushed away from the light, in shadow-map texels at "
                           "that caster. Larger hides acne and detaches contact shadows." ) )
        float ShadowBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Slope Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ),
                  Units( "texels" ),
                  Tooltip( "Extra bias of a caster the light grazes, in texels per unit of slope (clamped at "
                           "a slope of 1, 45 degrees - the one clamp every shadow caster has)." ) )
        float ShadowSlopeBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Resolution Scale" ), Category( "Shadows" ), Range( 0.125f, 4.0f ),
                  Units( "x" ), Advanced,
                  Tooltip( "Scales the atlas tiles this light asks for (its screen size decides the base); "
                           "the atlas still caps it." ) )
        float ShadowResolutionScale = 1.0f;

        PROPERTY( DisplayName( "Shadow Method" ), Category( "Shadows" ), Advanced,
                  Tooltip( "Shadows Stochastic only: Auto follows the view; Ray Query, Virtual Maps or Screen "
                           "trace this light's samples that way when the device runs it." ) )
        Common::Scalability::LightShadowMethod ShadowMethod = Common::Scalability::LightShadowMethod::Auto;

        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the light: a fraction of the screen "
                           "height at the receiver (0.1 = a tenth), or centimetres when In World Space. "
                           "0 = no contact shadows." ) )
        float ContactShadowLength = 0.0f;

        PROPERTY( DisplayName( "Contact Shadow Length In World Space" ), Category( "Contact Shadows" ),
                  Tooltip( "Contact Shadow Length is in world centimetres instead of a fraction of the screen." ) )
        bool ContactShadowLengthInWorldSpace = false;

        PROPERTY( DisplayName( "Contact Shadow Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion of a contact shadow cast by a mesh whose Cast Contact Shadow is on "
                           "(1 = black, 0 = none)." ) )
        float ContactShadowCastingIntensity = 1.0f;

        PROPERTY( DisplayName( "Contact Shadow Non Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion where the ray hits a mesh whose Cast Contact Shadow is off "
                           "(0 = such a mesh casts none)." ) )
        float ContactShadowNonCastingIntensity = 0.0f;

        PROPERTY( DisplayName( "Volumetric Scattering Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ),
                  Tooltip( "How much of this light the volumetric fog scatters (UE ULightComponent::"
                           "VolumetricScatteringIntensity). 0 = the light leaves no shaft or halo in fog." ) )
        float VolumetricScatteringIntensity = 1.0f;

        // UE ULocalLightComponent::IESTexture: the .dies profile (Assets::IesProfileAsset, an IESNA LM-63 .ies
        // imported, stored as {Guid, Path}) that shapes the light by
        // direction in its own frame (forward = the entity's -Z, horizontal 0 along its Y), normalised to the
        // profile's peak so Intensity stays the brightness. Runtime::IesProfileService packs it into the atlas.
        PROPERTY( DisplayName( "IES Profile" ), Category( "Light Profiles" ), Asset<IesProfileAsset>,
                  Tooltip( "An IES light profile (.dies, imported from a .ies) from the project content; "
                           "None = unshaped." ) )
        Assets::AssetHandle IesProfile;

        // UE ULocalLightComponent::bUseIESBrightness / IESBrightnessScale: the profile's own peak candela
        // (Assets::IesProfileAsset::PeakCandela, the photometric file's brightness) times the scale replaces
        // Intensity and its units; the light then emits what the luminaire's file states. Off, or no profile =
        // Intensity as authored.
        PROPERTY( DisplayName( "Use IES Brightness" ), Category( "Light Profiles" ), Advanced,
                  Tooltip( "Take the brightness from the IES profile (its peak candela x IES Brightness Scale) "
                           "instead of Intensity." ) )
        bool UseIesBrightness = false;

        PROPERTY( DisplayName( "IES Brightness Scale" ), Category( "Light Profiles" ), Range( 0.0f, 10.0f ),
                  Units( "x" ), Advanced, EditCondition( "UseIesBrightness" ),
                  Tooltip( "Scales the profile's peak candela when Use IES Brightness is on." ) )
        float IesBrightnessScale = 1.0f;

        // UE ULightComponent::LightFunctionMaterial / LightFunctionScale (LIGHT-FUNCTIONS, UE 5.3+ light function
        // atlas): a `Domain LightFunction` material rendered each frame into the light's tile of the view's atlas
        // (System::LightFunctionAtlas) and multiplied into the light's radiance (Mesh/LightFunctionAtlas.glslh).
        PROPERTY( DisplayName( "Light Function Material" ), Category( "Light Function" ), Asset<MaterialAsset>,
                  Tooltip( "UE Light Function Material: a material in the Light Function domain that filters "
                           "the light's colour by direction. None = no light function." ) )
        Assets::AssetHandle LightFunctionMaterial;

        PROPERTY( DisplayName( "Light Function Scale" ), Category( "Light Function" ), Length,
                  Range( 1.0f, 1.0e6f ),
                  Tooltip( "UE Light Function Scale: the size of the function's pattern in light space (cm); "
                           "the material's LightVector is the light-space position divided by it." ) )
        glm::vec3 LightFunctionScale = { 1024.0f, 1024.0f, 1024.0f };
    };

    struct RectLightComponent
    {
        COMPONENT( Key( "RectLight" ), Block( Data ), Run( ActorsAndUI ) )
        RectLightData Data;
    };
} // namespace Desert::ECS

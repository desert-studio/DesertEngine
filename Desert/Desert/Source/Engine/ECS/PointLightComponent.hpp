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

#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/LightUnits.hpp>

namespace Desert::ECS
{
    // LightFalloff and LightUnits (UE ELightUnits) live in Engine/ECS/LightUnits.hpp, next to the one conversion
    // the renderer applies (LightPhotometry::ShaderIntensity).

    struct PointLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE ULocalLightComponent::Intensity / IntensityUnits: the value in the units beside it (default 8 cd, as
        // UE). Switching the units in Details re-states the value so the brightness holds (EditorPostEdit); the
        // renderer reads it only through LightPhotometry::ShaderIntensity. Physical units need InverseSquare.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 100000.0f ), Summary,
                  Tooltip( "Brightness in Intensity Units. Candelas: 1 cd lights a surface 1 m away with 1 lux; "
                           "Lumens: total flux (a 60 W bulb ~800 lm, a street lamp ~2500 lm); EV100: 2^(EV-3) cd; "
                           "Unitless: the renderer's multiplier at Min Radius. Physical units apply to "
                           "Inverse Square falloff only." ) )
        float Intensity = 8.0f;

        PROPERTY( DisplayName( "Intensity Units" ), Category( "Light" ),
                  Tooltip( "What Intensity is measured in (UE Intensity Units). Changing it converts the value "
                           "so the light keeps its brightness." ) )
        LightUnits IntensityUnits = LightUnits::Candelas;

        PROPERTY( DisplayName( "Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary )
        float Radius = 1000.0f;

        // Inner radius where attenuation == 1 (a small emitter "source size"); falloff runs from here to
        // Radius. 0 = point source.
        PROPERTY( DisplayName( "Min Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Advanced )
        float MinRadius = 0.0f;

        // UE ULocalLightComponent::SourceRadius: the emitter is a sphere of this radius, not a point. Traced
        // shadows (MegaLights, Shadows Stochastic) aim each ray at a point of that sphere, so a shadow is sharp
        // at its caster and widens with the distance to the receiver. 0 = a point (a hard shadow).
        PROPERTY( DisplayName( "Source Radius" ), Category( "Light" ), Range( 0.0f, 1000.0f ), Length, Advanced )
        float SourceRadius = 0.0f;

        // UE ULocalLightComponent::SourceLength: the emitter is a capsule ("tube", a fluorescent tube) of
        // Source Radius around a segment of this length along the light's forward (the entity's -Z), centred on
        // the light. Its specular is a stretched highlight, its diffuse the segment's irradiance. 0 = a sphere.
        PROPERTY( DisplayName( "Source Length" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Advanced )
        float SourceLength = 0.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Light" ), Advanced )
        LightFalloff Falloff = LightFalloff::InverseSquare; // UE bUseInverseSquaredFalloff, on by default

        PROPERTY( DisplayName( "Show Radius" ), Category( "Light" ) )
        bool ShowRadius = false;

        // THE POINT LIGHT'S SHADOW (UE: ULightComponent::CastShadows / ShadowBias / ShadowSlopeBias /
        // ShadowResolutionScale). Drawn into the view's local-light shadow atlas (Graphic/LocalShadows.hpp) and
        // read by the deferred composite. The two biases are applied by every CASTER of the light's views (the
        // meshes and the terrain alike, Graphic::LocalCasterBias), in SHADOW-MAP TEXELS at the caster, not
        // centimetres: one texel of a perspective map grows with the distance from the light, so a fixed length
        // would be too much next to the lamp and too little across the room.
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
                  Tooltip( "Scales the atlas tile this light asks for (its screen size decides the base); "
                           "the atlas still caps it." ) )
        float ShadowResolutionScale = 1.0f;

        // UE ULightComponent::MegaLightsShadowMethod: under Shadows Stochastic, the source MegaLights tests this
        // light's samples with (Common::Scalability::LightShadowMethod); Auto = the view's. Clamped to what the
        // device runs - DesertCtl state render.shadows.lights names the effective one.
        PROPERTY( DisplayName( "Shadow Method" ), Category( "Shadows" ), Advanced,
                  Tooltip( "Shadows Stochastic only: Auto follows the view; Ray Query, Virtual Maps or Screen "
                           "trace this light's samples that way when the device runs it." ) )
        Common::Scalability::LightShadowMethod ShadowMethod = Common::Scalability::LightShadowMethod::Auto;

        // CONTACT SHADOWS (UE: ULightComponent::ContactShadowLength / bContactShadowLengthInWS, the same default 0
        // = off). A short screen-space ray toward the light through the scene depth, marched by the deferred
        // composite (Common/ContactShadow.glslh), that grounds the small and near-contact detail the shadow map is
        // too coarse or too biased to hold. Needs Cast Shadows; its step count is Scalability
        // Shadows.ContactShadowSteps.
        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the light: a fraction of the screen "
                           "height at the receiver (0.1 = a tenth), or centimetres when In World Space. "
                           "0 = no contact shadows." ) )
        float ContactShadowLength = 0.0f;

        PROPERTY( DisplayName( "Contact Shadow Length In World Space" ), Category( "Contact Shadows" ),
                  Tooltip( "Contact Shadow Length is in world centimetres instead of a fraction of the screen." ) )
        bool ContactShadowLengthInWorldSpace = false;

        // UE ULightComponent::ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: how dark the
        // contact shadow is where the ray hits a mesh that casts contact shadows (its Cast Contact Shadow flag),
        // and where it hits one that does not. UE's defaults: 1 and 0.
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

    struct PointLightComponent
    {
        COMPONENT( Key( "PointLight" ), Block( Data ), Run( ActorsAndUI ) )
        PointLightData Data;
    };
} // namespace Desert::ECS

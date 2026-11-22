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

namespace Desert::ECS
{
    // ON THE TWO PAIRS OF SUN NUMBERS. Colour x Intensity here is the ILLUMINANCE arriving at scene
    // surfaces — what every lit surface integrates. SkyAtmosphereData::SunColor x SunIntensity is the
    // RADIANCE of the sky and of the solar disk — what the camera sees when it looks up. Two different
    // quantities with different consumers, not one value stored twice: neither is derived from the other,
    // and no code path reads one where it means the other. See SkyAtmosphereComponent.hpp.
    struct DirectionalLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature,
                  Tooltip( "Tint of the sun light (UE). With a Physical Atmosphere sky this is also the sun "
                           "the sky scatters and the disc you see." ) )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE UDirectionalLightComponent::Intensity: ILLUMINANCE in lux on a surface facing the light (a clear
        // midday sun is ~100 000 lux, an overcast day ~1 000, a full moon ~0.25). The renderer multiplies the BRDF
        // by Color x Intensity directly - lux IS its unit, the same unit a point light's candelas reach a surface
        // in (ECS::LightPhotometry) - and the exposure range (PostProcessSettings, EV100) is what makes a physical
        // value look right. Scenes authored before units existed stated the same number; it was always lux.
        // As in UE it is the OUTER-SPACE illuminance of the atmosphere sun: the Physical Atmosphere sky
        // scatters exactly this (Graphic::SkySunInputs), and the ground receives it times the transmittance.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 150000.0f ), Summary,
                  Units( "lux" ),
                  Tooltip( "Illuminance in lux on a surface facing the light (UE): ~100000 clear sun, ~1000 "
                           "overcast, ~0.25 full moon. With a Physical Atmosphere sky this also drives the "
                           "sky's brightness and the sun disc." ) )
        float Intensity = 1.0f;

        // Defaults to true so that every scene authored before this field existed keeps working: a field
        // missing from the file keeps its C++ default, so the one directional light such a scene has
        // becomes its atmosphere sun with no migration.
        PROPERTY( DisplayName( "Atmosphere Sun Light" ), Category( "Atmosphere" ),
                  Tooltip( "This light drives the sky and the sky's IBL bake." ) )
        bool AtmosphereSunLight = true;

        PROPERTY( DisplayName( "Atmosphere Sun Light Index" ), Category( "Atmosphere" ), Range( 0, 1 ),
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "0 = the sun, 1 = a second atmosphere light such as the moon (UE). Both light the "
                           "sky, draw a disc and light the scene; the time-of-day clock and the cascaded "
                           "shadows belong to index 0." ) )
        int AtmosphereSunLightIndex = 0;

        // UE's "Affected By Atmosphere Transmittance", same name, same default (ON): in
        // SkyModel::PhysicalAtmosphere the colour above is multiplied by the atmosphere's transmittance
        // toward the sun at ground level, so a sunset reddens and dims the light on geometry by the same
        // law that reddens the sky behind it. Consumed by SceneRenderer::OnUpdate through the
        // AtmosphereEnv::SunTransmittanceAtGround the SkyboxRenderer publishes.
        //
        // Switching it off returns the light to exactly its authored colour — the artist's escape from a
        // physical sun, and the ONLY way to keep an authored colour in the physical model.
        // SkyModel::ArtisticGradient ignores this field entirely: there the sky's radiance and the
        // surface illuminance are independent by documented design (SkyAtmosphereComponent.hpp), and
        // this coupling does not exist to be switched off.
        PROPERTY( DisplayName( "Affected By Atmosphere Transmittance" ), Category( "Atmosphere" ),
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "Multiply this light's colour by the atmosphere's transmittance toward the "
                           "sun at ground level, so it reddens and dims at sunset. Physical Atmosphere "
                           "only." ) )
        bool AffectedByAtmosphereTransmittance = true;

        // UE UDirectionalLightComponent::AtmosphereSunDiskColorScale, same name, same default (white): scales
        // ONLY the disc the physical sky draws for this sun — not the sky's scattering, not the surfaces.
        // Scenes from before v57, whose sky had its own sun numbers, state here what their disc was.
        PROPERTY( DisplayName( "Atmosphere Sun Disk Color Scale" ), Category( "Atmosphere" ), Color,
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "Scales the sun disc the Physical Atmosphere sky draws for this light (UE)." ) )
        glm::vec3 AtmosphereSunDiskColorScale = glm::vec3( 1.0f );

        // ---- Light Shafts (UE's category, UE's names, UE's defaults) ----------------------------------
        // The screen-space sun streaks: a bright-pass of the HDR scene around the sun's position on
        // screen, radially blurred toward it and added back before tonemapping. Occlusion is inherited
        // from the scene colour itself — whatever stands in front of the sun composites with its real
        // transmittance, so the shafts exist exactly where the sun breaks through. Consumed by
        // System::LightShaftRenderer via the SunLightFx slice of the ProceduralSkyCommand; only the
        // atmosphere sun's values are read.
        PROPERTY( DisplayName( "Light Shaft Bloom" ), Category( "Light Shafts" ),
                  Tooltip( "Radial streaks of the sun's light through gaps in whatever occludes it, "
                           "added to the scene before tonemapping." ) )
        bool LightShaftBloom = false;

        PROPERTY( DisplayName( "Bloom Scale" ), Category( "Light Shafts" ), Range( 0.0f, 10.0f ),
                  EditCondition( "LightShaftBloom" ), Tooltip( "Overall strength of the light-shaft bloom." ) )
        float BloomScale = 0.2f;

        PROPERTY( DisplayName( "Bloom Threshold" ), Category( "Light Shafts" ), Range( 0.0f, 4.0f ),
                  EditCondition( "LightShaftBloom" ),
                  Tooltip( "Scene luminance below this contributes nothing to the shafts." ) )
        float BloomThreshold = 0.0f;

        PROPERTY( DisplayName( "Bloom Max Brightness" ), Category( "Light Shafts" ), Range( 0.0f, 100.0f ),
                  EditCondition( "LightShaftBloom" ),
                  Tooltip( "Cap on the energy a single pixel may contribute — stops one blown-out pixel "
                           "from owning the whole streak." ) )
        float BloomMaxBrightness = 100.0f;

        PROPERTY( DisplayName( "Bloom Tint" ), Category( "Light Shafts" ), Color,
                  EditCondition( "LightShaftBloom" ), Tooltip( "Tint of the light-shaft streaks." ) )
        glm::vec3 BloomTint = glm::vec3( 1.0f );

        // THE CASCADED SHADOW MAPS OF THIS LIGHT (UE: UDirectionalLightComponent's Cascaded Shadow Maps
        // section). They were SceneSettings until SET1 — a level-wide switch for the shadows of the one
        // light that has any. Graphic::ResolveViewSettings reads them from the light it elects, the same
        // one Scene::OnUpdate shades with.
        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Cascaded Shadow Maps" ) )
        bool CastShadows = true;

        // THE SUN'S ANGULAR DIAMETER (UE: UDirectionalLightComponent::LightSourceAngle, the same default). It is
        // what makes a shadow soft: the penumbra grows by tan(angle / 2) per centimetre between the occluder and
        // the receiver, so a shadow is contact-sharp where the caster meets the ground and widens with distance
        // (the cascades' PCSS, Mesh/CascadedShadow.glslh).
        PROPERTY( DisplayName( "Source Angle" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 10.0f ),
                  Units( "deg" ),
                  Tooltip( "Angular diameter of the sun in degrees. Larger = softer shadows away from their "
                           "casters; 0 = hard shadows everywhere." ) )
        float SourceAngle = 0.5357f;

        // THE CASTER'S DEPTH BIAS, IN WORLD CENTIMETRES (UE: ULightComponent::ShadowBias / ShadowSlopeBias,
        // UDirectionalLightComponent::ShadowCascadeBiasDistribution). Applied where the caster writes its
        // depth (MeshRenderer::FitShadowCascades -> the caster matrix and CameraUB.ShadowSlopeBias), never
        // at the receiver: a bias in a cascade's NDC depth was 26 cm in the nearest cascade and 10 m in the
        // farthest, and erased every awning, lamp and window frame past the first. Both lengths are what
        // the NEAREST cascade gets; Distribution scales the farther ones by their texel size relative to it.
        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 50.0f ),
                  Units( "cm" ),
                  Tooltip( "How far the caster's depth is pushed away from the sun, in the nearest cascade. "
                           "Larger hides self-shadow acne and detaches contact shadows." ) )
        float ShadowBias = 1.0f;

        PROPERTY( DisplayName( "Shadow Slope Bias" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 50.0f ),
                  Units( "cm" ),
                  Tooltip( "Extra depth bias of a caster surface at 45 degrees to the sun (and steeper, "
                           "clamped there), in the nearest cascade." ) )
        float ShadowSlopeBias = 2.0f;

        PROPERTY( DisplayName( "Cascade Bias Distribution" ), Category( "Cascaded Shadow Maps" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "0 = every cascade gets the same centimetres; 1 = a cascade's bias grows with its "
                           "texel size, as UE's Shadow Cascade Bias Distribution." ) )
        float ShadowCascadeBiasDistribution = 1.0f;

        PROPERTY( DisplayName( "Cascade Split Lambda" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 1.0f ),
                  Tooltip( "0 = uniform cascade splits, 1 = logarithmic; UE's Distribution Exponent plays "
                           "the same part." ) )
        float CascadeSplitLambda = 0.6f;

        // CONTACT SHADOWS (UE: ULightComponent::ContactShadowLength / bContactShadowLengthInWS, the same default 0
        // = off). A short screen-space ray toward the light through the scene depth, marched by the deferred
        // composite (Common/ContactShadow.glslh), that grounds the small and near-contact detail the shadow map is
        // too coarse or too biased to hold. Needs Cast Shadows; its step count is Scalability
        // Shadows.ContactShadowSteps.
        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the sun: a fraction of the screen "
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
    };

    struct DirectionLightComponent
    {
        COMPONENT( Key( "DirectionLight" ), Block( Data ), Run( ActorsAndUI ) )
        DirectionalLightData Data;
    };
} // namespace Desert::ECS

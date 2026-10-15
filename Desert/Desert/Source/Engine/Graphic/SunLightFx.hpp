#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The atmosphere sun light's RENDER EFFECTS, in transport form — the evaluated slice of
    // ECS::DirectionalLightData that the renderer consumes beyond plain surface illumination. Travels
    // on the ProceduralSkyCommand next to the sun direction, because every field here is a property OF
    // that sun: shafts without the sun that casts them are not a thing.
    //
    // Same closed-struct rule as AtmosphereEnv: values, never the authoring component.
    struct SunLightFx
    {
        // UE's "Light Shafts" category, verbatim semantics: a bright-pass of the scene around the sun's
        // screen position, radially blurred toward it, added back before tonemapping.
        bool      LightShaftBloom    = false;
        float     BloomScale         = 0.2f;
        float     BloomThreshold     = 0.0f;
        float     BloomMaxBrightness = 100.0f;
        glm::vec3 BloomTint          = glm::vec3( 1.0f );

        // The sun light's OWN illuminance before the atmosphere touches it — the authored Color times
        // Intensity of the directional light that was chosen as the atmosphere sun. UE calls this the
        // outer-space illuminance in the physical model, and in that model it is ALSO the sky's sun: the
        // Sky-View LUT, the aerial perspective, the distant sky light, the IBL bake and the disc integrate
        // it (SkySunInputs, SkyPayload.hpp). Only the artistic gradient keeps the sky component's own
        // SunColor x SunIntensity, a palette value with no unit.
        //
        // It travels here with the other properties of the chosen sun so that a consumer which needs the
        // light's illuminance — the fog's directional lobe — reads the sun the sky is driving, and not
        // whichever directional light happened to be first in the registry.
        glm::vec3 OuterSpaceIlluminance = glm::vec3( 0.0f );

        // UE's Atmosphere Sun Disk Color Scale: scales the disc the physical sky draws for this light, and
        // nothing else (not the sky's scattering, not the surfaces).
        glm::vec3 SunDiskColorScale = glm::vec3( 1.0f );

        // UE's "Affected By Atmosphere Transmittance": whether this sun's colour is multiplied by the
        // atmosphere's transmittance at ground level (physical model only — see the component).
        //
        // FALSE HERE, TRUE ON THE COMPONENT, and the difference is the point: this struct is reset to
        // its defaults whenever no directional light was chosen as the atmosphere sun, and "no sun was
        // chosen" must not silently redden a light that the sky is not driving. A chosen sun overwrites
        // this with its own authored value, which defaults to on as UE ships it.
        bool AffectedByAtmosphereTransmittance = false;
    };
} // namespace Desert::Graphic

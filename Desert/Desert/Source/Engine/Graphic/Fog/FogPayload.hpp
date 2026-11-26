#pragma once

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Fog/FogLayout.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // The block (FogLayout.hpp) is a buffer of each frame's graph (HeightFogRenderer::DeclareFrameNodes, sized by
    // the struct), found BY ITS NAME (kSceneFogParamsBlockName) in every layout that declares it - the fog compute
    // at kFogParamsBinding, a surface cell at Core::kSceneFogParamsBinding.

    // The per-dispatch push (the camera's InverseViewProjection) is HeightFog.shader's block,
    // ShaderPush::HeightFog: the camera position, the aerial-perspective extent, the two gates and the pre-exposure
    // are in FogGpuPayload (Camera, View, Gates), which the translucent surfaces read too.

    // The camera's forward axis from a world -> view matrix (the camera looks down -z of its view space): the axis
    // a froxel slice is measured along, by the volumetric fog that fills the grid and by every reader of it.
    inline glm::vec3 FogCameraForward( const glm::mat4& view )
    {
        return -glm::normalize( glm::vec3( view[0][2], view[1][2], view[2][2] ) );
    }

    // One world unit is one centimetre (Common::Units), so a kilometre is 100 000 of them.
    inline constexpr float kFogWorldUnitsPerKm = 100000.0f;

    // UE authors FogDensity and FogHeightFalloff "per 1000 cm" (its scene proxy divides both by 1000
    // before they reach the shader — research doc section 3.2). Per kilometre that is x100000 / 1000.
    inline constexpr float kFogUEUnitsPerKm = 100.0f;

    /**
     * Fill the GPU block from the component, the atmosphere and the fog entity's height. Pure: no GPU,
     * no globals, no clock — the HeightFog tests drive it directly.
     *
     * @param fogHeightWorldY the fog entity's TransformComponent Y, world units — the layer's floor,
     *                        never a field of the component (one owner, like UE's component transform).
     *
     * The two atmosphere couplings, both of them now scoped to the sky model by the SAME gate — a
     * non-null AtmosphereEnv::DistantSkyLight, which is published in SkyModel::PhysicalAtmosphere and
     * nowhere else. One gate rather than two so the fog cannot end up half physical:
     *   * DIRECTIONAL: UE feeds the lobe the sun's POST-TRANSMITTANCE illuminance
     *     (AtmosphereLightIlluminanceOnGroundPostTransmittance) scaled by the sky's
     *     HeightFogContribution. In PhysicalAtmosphere ours is now that quantity, at contribution 1:
     *     AtmosphereEnv::SunIlluminanceOnGround, the directional LIGHT's Color x Intensity times the
     *     atmosphere's transmittance toward the sun. In ArtisticGradient it stays
     *     AtmosphereEnv::SunIrradiance, the sky's own elevation-tinted sun, which is what the gradient's
     *     fog was authored against and what keeps a gradient scene bit for bit what it was.
     *
     *     THE TWO DIFFER BY MORE THAN AN ORDER OF MAGNITUDE and that is the point, not a defect: the
     *     sky's SunIntensity says how bright the disc LOOKS (22 in the showcase scenes), the light's
     *     Intensity says how brightly the sun LIGHTS THINGS (1 by default). A fog whose sun lobe is
     *     twenty times the illuminance actually falling on the scene is a glow with no light behind it.
     *     Physical scenes therefore author DirectionalInscatteringLuminance to say how strong they want
     *     the lobe, exactly as UE scenes do.
     *   * AMBIENT: the average sky, and WHERE it comes from follows the sky model, because the two
     *     models have different ambients and neither is a stand-in for the other:
     *       - SkyModel::PhysicalAtmosphere — the DISTANT SKY LIGHT texel, marched every frame
     *         (AtmosphereEnv::DistantSkyLight, UE's Distant Sky Light LUT). It cannot be folded in
     *         here: it lives on the GPU, so what this packer carries is the artist's scale and a gate,
     *         and the shader does the multiply.
     *       - SkyModel::ArtisticGradient — the mean of the dome and ground-bounce radiance, the
     *         gradient's own ambient, so a gradient scene's fog is bit for bit what it was.
     */
    inline FogGpuPayload PackFogParams( const ECS::ExponentialHeightFogData& data, const AtmosphereEnv& atmosphere,
                                        float fogHeightWorldY )
    {
        const float heightKm = fogHeightWorldY / kFogWorldUnitsPerKm;

        // Repaired at the boundary rather than trusted, like every payload packer: each of these is one
        // hand-edited scene file away from a negative density or a zero-degree pow lobe.
        const float density0 = glm::max( data.FogDensity, 0.0f ) * kFogUEUnitsPerKm;
        const float falloff0 = glm::max( data.FogHeightFalloff, 0.0f ) * kFogUEUnitsPerKm;
        const float density1 = glm::max( data.SecondFogDensity, 0.0f ) * kFogUEUnitsPerKm;
        const float falloff1 = glm::max( data.SecondFogHeightFalloff, 0.0f ) * kFogUEUnitsPerKm;

        const float maxOpacity = glm::clamp( data.FogMaxOpacity, 0.0f, 1.0f );

        // The physical average sky is a GPU texel, so the ambient reaches the shader as two halves: the
        // scale here, the value there. The handle being non-null is the model switch — AtmosphereEnv
        // publishes it in PhysicalAtmosphere and only there — and it gates the DIRECTIONAL lobe too, so
        // both couplings change model together.
        const bool physical = atmosphere.Valid && atmosphere.DistantSkyLight != nullptr;

        glm::vec3 ambient{ 0.0f };
        glm::vec3 sunLobe = data.DirectionalInscatteringLuminance;
        glm::vec3 sunDir{ 0.0f, 1.0f, 0.0f };
        if ( atmosphere.Valid )
        {
            if ( !physical )
            {
                ambient = data.SkyAtmosphereAmbientContributionColorScale * 0.5f *
                          ( atmosphere.ZenithRadiance + atmosphere.GroundRadiance );
            }
            sunLobe += physical ? atmosphere.SunIlluminanceOnGround : atmosphere.SunIrradiance;
            sunDir = atmosphere.SunDirection;
        }
        else
        {
            // Without an atmosphere there is no sun to build the lobe around: an authored directional
            // colour pointed at an invented direction would be a glow with no light behind it, so the
            // whole term is dropped rather than guessed.
            sunLobe = glm::vec3( 0.0f );
        }

        FogGpuPayload p{};
        p.SunDirection = glm::vec4( sunDir, glm::max( data.DirectionalInscatteringExponent, 1.0f ) );
        p.Directional  = glm::vec4( sunLobe, glm::max( data.DirectionalInscatteringStartDistance, 0.0f ) /
                                                  kFogWorldUnitsPerKm );
        p.Inscattering = glm::vec4( data.FogInscatteringLuminance + ambient, 1.0f - maxOpacity );
        p.Layer0 =
             glm::vec4( density0, falloff0, heightKm, glm::max( data.StartDistance, 0.0f ) / kFogWorldUnitsPerKm );
        p.Layer1 = glm::vec4( density1, falloff1, heightKm + data.SecondFogHeightOffset / kFogWorldUnitsPerKm,
                              glm::max( data.FogCutoffDistance, 0.0f ) / kFogWorldUnitsPerKm );
        p.Ambient = glm::vec4( data.SkyAtmosphereAmbientContributionColorScale, physical ? 1.0f : 0.0f );
        return p;
    }
} // namespace Desert::Graphic

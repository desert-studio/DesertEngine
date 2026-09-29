#pragma once

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/Graphic/AtmosphereEnv.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * The GPU side of ExponentialHeightFogData, and the ONLY place the component is turned into bytes.
     *
     * The GLSL half of this layout is the block in Engine/Content/Shaders/Common/FogParams.glslh,
     * member for member and in this order. The static_asserts below make a divergence a build error
     * instead of a frame in which every parameter after the inserted one is read from the wrong offset.
     *
     * Units: KILOMETRES and per-kilometre coefficients throughout — this packer is where the component's
     * world-unit distances and UE-unit densities are converted, exactly once ("kilometres once, inside
     * the shader"; the closed form's precision argument is in HeightFog.glslh).
     */
    struct FogGpuPayload
    {
        glm::vec4 SunDirection; // xyz toward sun (normalized), w = DirectionalInscatteringExponent
        glm::vec4 Directional;  // rgb lobe colour, w = directional start distance (km)
        glm::vec4 Inscattering; // rgb fog colour (sky ambient folded in), w = 1 - FogMaxOpacity
        glm::vec4 Layer0;       // x density /km, y falloff /km, z fog height (km), w start distance (km)
        glm::vec4 Layer1;       // x density /km, y falloff /km, z fog height (km), w cutoff distance (km)

        // The SKY AMBIENT the fog adds to its in-scattering colour, in the form the GPU needs it:
        //   rgb = SkyAtmosphereAmbientContributionColorScale, the artist's per-channel scale;
        //   w   = 1 when the distant-sky-light texel is bound and must be sampled, 0 otherwise.
        //
        // The VALUE it scales is not here because it does not exist on the CPU: the physical average sky
        // is one texel produced by Programs/Sky/SkyDistantLight.shader and read by the fog pass in the
        // same frame, exactly as UE's height fog reads its Distant Sky Light LUT. With w = 0 the shader
        // adds nothing and Inscattering below carries whatever ambient the model does have — see
        // PackFogParams.
        glm::vec4 Ambient;
    };

    static_assert( offsetof( FogGpuPayload, SunDirection ) == 0 );
    static_assert( offsetof( FogGpuPayload, Directional ) == 16 );
    static_assert( offsetof( FogGpuPayload, Inscattering ) == 32 );
    static_assert( offsetof( FogGpuPayload, Layer0 ) == 48 );
    static_assert( offsetof( FogGpuPayload, Layer1 ) == 64 );
    static_assert( offsetof( FogGpuPayload, Ambient ) == 80 );
    static_assert( sizeof( FogGpuPayload ) == 96, "Six vec4s, nothing else — the shader reads exactly this." );

    // Already a multiple of std430's 16-byte block alignment, so the buffer size IS the struct size.
    inline constexpr uint32_t kFogPayloadBytes = sizeof( FogGpuPayload );

    // The bindings of the fog compute pass. SetStorageBuffer / SetInput take these as explicit
    // arguments and never consult the shader's own reflection — kFogParamsBinding must equal
    // FOG_PARAMS_BINDING in Common/FogParams.glslh (the SkyPayload.hpp binding-number trap).
    inline constexpr uint32_t kFogOutputBinding     = 0; // the RGBA16F fog image the pass writes
    inline constexpr uint32_t kFogParamsBinding     = 1;
    inline constexpr uint32_t kFogSceneDepthBinding = 2;
    // The sky's camera aerial-perspective volume, which this pass composes ITSELF OVER. ALWAYS bound,
    // even in a scene with no atmosphere at all: a declared sampler with no image is an invalid
    // descriptor set, not an unused one. What varies
    // is FogPush::AerialPerspective.z, which is 0 when there is no volume — and then the shader never
    // reads the binding and composes the exact identity instead.
    inline constexpr uint32_t kFogAerialPerspectiveBinding = 3;
    // The sky's DISTANT SKY LIGHT — the one texel holding the average sky radiance. Bound on the same
    // terms as the volume above (always bound, gated by FogGpuPayload::Ambient.w) and for the same
    // reason: a declared sampler with no image is an invalid descriptor set, not an unused one.
    inline constexpr uint32_t kFogDistantSkyLightBinding = 4;

    /**
     * Per-dispatch data: everything that changes with the CAMERA rather than with the fog settings.
     * Rides in the push constant because ComputePipeline has no SetUniformBuffer, and a second storage
     * buffer for one matrix would be a second per-frame upload.
     */
    struct FogPush
    {
        glm::mat4 InverseViewProjection;
        glm::vec4 CameraPosition; // xyz = world units, w unused (vec4 keeps the std430 block unambiguous)

        // The aerial perspective this pass composes itself over, and the two switches that decide what
        // the dispatch actually evaluates. Both gates are here rather than in the fog parameter block
        // because both are per-VIEW facts about this frame, not authored fog:
        //   x = the AP volume's far extent, kilometres  (AtmosphereEnv::AerialPerspectiveDepthKm)
        //   y = the read-side view-distance scale       (AtmosphereEnv::AerialPerspectiveViewDistanceScale)
        //   z = 1 when there is an AP volume to sample, 0 otherwise
        //   w = 1 when the height fog itself is enabled, 0 otherwise
        // Either gate at 0 makes its half of the composite the exact arithmetic identity, which is what
        // lets one pass serve fog-without-sky, sky-without-fog and both, with no permutation.
        glm::vec4 AerialPerspective;
    };

    static_assert( sizeof( FogPush ) == 96 );

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

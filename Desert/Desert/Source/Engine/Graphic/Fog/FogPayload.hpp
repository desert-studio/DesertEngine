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
     * The GLSL half of this layout is the block in Editor/Resources/Shaders/Common/FogParams.glslh,
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

        // The VOLUMETRIC FOG this pass composes under the analytic fog (VolumetricFogRenderer's integrated volume,
        // UE's CombineVolumetricFog). Not authored data: PackFogParams leaves both zero (no volume) and
        // HeightFogRenderer fills them from the frame's VolumetricFogFrameResult when the volume ran.
        //   Volumetric0: x = froxel edge (render pixels), y/z = froxels across/down, w = 1 when the volume is
        //   bound Volumetric1: x/y/z = B, O, S of slice = log2(depth * B + O) * S (depth in cm), w = slices
        glm::vec4 Volumetric0;
        glm::vec4 Volumetric1;

        // The per-VIEW facts of this frame, in the block (not the opaque pass's push constant) because the
        // translucent surfaces evaluate the same fog at their own depth from this same buffer
        // (Common/ViewFog.glslh): one block, read by the opaque composite and by every translucent draw.
        // Not authored data: PackFogParams leaves both zero and HeightFogRenderer fills them every frame.
        //   View:  xy = the scene view's size (render pixels), z = the AP volume's far extent (km,
        //          AtmosphereEnv::AerialPerspectiveDepthKm), w = the read-side view-distance scale
        //          (AtmosphereEnv::AerialPerspectiveViewDistanceScale)
        //   Gates: x = 1 when there is an AP volume to sample, y = 1 when the height fog itself is enabled.
        //          Either at 0 makes its half the exact arithmetic identity (fog-without-sky, sky-without-fog
        //          and both from one shader, no permutation); both 0 = no fog this frame, every reader leaves
        //          its colour untouched.
        //   Forward: xyz = the camera's forward (normalized, world): a point's froxel slice is its distance
        //          along it, the same for the opaque pixel and the translucent fragment.
        //   Camera: xyz = the camera's position (world units): where every fog integral starts.
        glm::vec4 View;
        glm::vec4 Gates;
        glm::vec4 Forward;
        glm::vec4 Camera;
    };

    static_assert( offsetof( FogGpuPayload, SunDirection ) == 0 );
    static_assert( offsetof( FogGpuPayload, Directional ) == 16 );
    static_assert( offsetof( FogGpuPayload, Inscattering ) == 32 );
    static_assert( offsetof( FogGpuPayload, Layer0 ) == 48 );
    static_assert( offsetof( FogGpuPayload, Layer1 ) == 64 );
    static_assert( offsetof( FogGpuPayload, Ambient ) == 80 );
    static_assert( offsetof( FogGpuPayload, Volumetric0 ) == 96 );
    static_assert( offsetof( FogGpuPayload, Volumetric1 ) == 112 );
    static_assert( offsetof( FogGpuPayload, View ) == 128 );
    static_assert( offsetof( FogGpuPayload, Gates ) == 144 );
    static_assert( offsetof( FogGpuPayload, Forward ) == 160 );
    static_assert( offsetof( FogGpuPayload, Camera ) == 176 );
    static_assert( sizeof( FogGpuPayload ) == 192, "Twelve vec4s, nothing else — the shader reads exactly this." );

    // The block is a buffer of each frame's graph (HeightFogRenderer::DeclareFrameNodes, sized by the struct:
    // already a multiple of std430's 16-byte alignment), found BY ITS NAME (kSceneFogParamsBlockName) in every
    // layout that declares it - the fog compute at FOG_PARAMS_BINDING 1, a surface cell at
    // Core::kSceneFogParamsBinding - so no binding number is spelled on this side.

    /**
     * Per-dispatch data: everything that changes with the CAMERA rather than with the fog settings.
     * Rides in the push constant because ComputePipeline has no SetUniformBuffer, and a second storage
     * buffer for one matrix would be a second per-frame upload.
     */
    struct FogPush
    {
        glm::mat4 InverseViewProjection;
        // The camera position, the aerial-perspective extent and the two gates are in FogGpuPayload (Camera, View,
        // Gates): the translucent surfaces read them too, and they have the block, not this push constant.
    };

    static_assert( sizeof( FogPush ) == 64 );

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

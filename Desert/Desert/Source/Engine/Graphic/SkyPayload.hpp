#pragma once

#include <Engine/Graphic/SkyLayout.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // THE sky parameter block is SkyGpuPayload in SkyLayout.hpp: Tools/ShaderProtocolGen writes its GLSL (SkyPacked)
    // and the buffer every sky pass binds, so the two sides cannot drift. No shader reads a field of it directly —
    // they call the unpack helpers in Common/Atmosphere.glslh. One layout serves the sky graphics pass, the
    // atmosphere LUT passes and the IBL bake compute dispatch.
    inline constexpr uint32_t kSkyPackedVec4Count =
         static_cast<uint32_t>( sizeof( SkyGpuPayload ) / sizeof( glm::vec4 ) );

    // THE PHYSICAL SKY'S SUN, as UE has it: the atmosphere sun LIGHT's own numbers, not the sky
    // component's. UE's sky atmosphere has no sun intensity of its own — the Sky-View LUT, the aerial
    // perspective, the distant sky light and the disc all integrate the directional light's outer-space
    // illuminance (Color x Intensity, lux) and the atmosphere contributes only its transmittance. The
    // artistic gradient is not physical and keeps the sky component's SunColor x SunIntensity.
    struct SkySunInputs
    {
        // The atmosphere sun light's Color x Intensity (lux) before the atmosphere — SunLightFx's own.
        glm::vec3 OuterSpaceIlluminance{ 0.0f };
        // UE UDirectionalLightComponent::AtmosphereSunDiskColorScale: the disc only, never the sky.
        glm::vec3 DiskColorScale{ 1.0f };
        // Graphic::SkyLuminanceCeiling for this frame's exposure (AutoExposureRules.hpp).
        float LuminanceCeiling = 0.0f;
    };

    // Size the buffer is created with, and the size every writer must hand to SetData.
    inline constexpr uint32_t kSkyPayloadBytes =
         kSkyPackedVec4Count * static_cast<uint32_t>( sizeof( glm::vec4 ) );

    // The atmosphere-LUT compute passes name their LUT images by shader name in their binding block
    // (SkyboxRenderer::DeclareAtmosphereLutNodes), so their outputs have no number here.
    // LUT INPUT bindings, shared by every compute consumer (SkyMultiScatterLut reads the transmittance
    // at 2; SkyViewLut and BakeProceduralSky read the transmittance at 2 and the multi-scatter at 3).
    inline constexpr uint32_t kSkyTransmittanceLutBinding = 2;
    inline constexpr uint32_t kSkyMultiScatterLutBinding  = 3;
    // The cloud layer's shadow on the air (UE VolumetricCloud "Shadow On Atmosphere"), read by the two per-view
    // fills (SkyViewLut, SkyAerialPerspectiveLut): the map at 4 (a pass parameter, CloudShadowMapOrWhite) and its
    // 80-byte CloudShadowUniforms block at 5, a storage buffer SkyboxRenderer owns (the AP push is 96 bytes, so
    // the block cannot ride in it).
    inline constexpr uint32_t kSkyCloudShadowMapBinding = 4;
    inline constexpr uint32_t kSkyCloudShadowBinding    = 5;

    // The Sky-View LUT extent — Hillaire's 192x104, mirrored by SKY_VIEW_LUT_WIDTH/HEIGHT in
    // Common/SkyScattering.glslh (the sub-texel remap bakes the numbers into every read and write, so
    // the two sides must state the same extent; the SkyScattering tests pin the shader-side pair).
    inline constexpr uint32_t kSkyViewLutWidth  = 192;
    inline constexpr uint32_t kSkyViewLutHeight = 104;

    // The SkyViewLut pass's push block is ShaderPush::SkyViewLut (generated from SkyViewLut.shader): the LUT
    // depends on the camera (its altitude picks the horizon warp), and the camera is the one per-frame quantity
    // the shared payload deliberately does not carry.

    // The camera aerial-perspective volume's extent — Hillaire's 32x32x16, mirrored by
    // SKY_AP_VOLUME_WIDTH/HEIGHT/DEPTH in Common/SkyScattering.glslh. Both sides must state the same
    // three numbers: the slice mapping's texel-centre remap bakes the depth into every write and every
    // read, and the x/y remap does the same for the screen extent.
    inline constexpr uint32_t kAerialPerspectiveWidth  = 32;
    inline constexpr uint32_t kAerialPerspectiveHeight = 32;
    inline constexpr uint32_t kAerialPerspectiveDepth  = 16;
    // The fill's workgroup (Programs/Sky/SkyAerialPerspectiveLut.shader, LocalSize(4, 4, 16), AP_GROUP_COLUMNS /
    // AP_GROUP_SLICES): 4x4 froxel columns x the WHOLE depth, because a column's slices are chained in one
    // group's shared memory. The group's z extent is therefore kAerialPerspectiveDepth itself.
    inline constexpr uint32_t kAerialPerspectiveGroupColumns = 4;
    static_assert( kAerialPerspectiveGroupColumns * kAerialPerspectiveGroupColumns * kAerialPerspectiveDepth <=
                        1024,
                   "one aerial-perspective group holds 4x4 columns x every slice; Vulkan guarantees 128, "
                   "every desktop GPU 1024 invocations" );

    // The distant sky light's extent, and the texel it holds. ONE march of 64 directions reduced once
    // (Programs/Sky/SkyDistantLight.shader):
    //   x = 0  the FULL-SPHERE mean — UE's Distant Sky Light, read by the height fog, which is lit from
    //          every direction at once and has no ground term of its own.
    // The shader indexes this by number, so the number lives here and not in a shader comment.
    inline constexpr uint32_t kDistantLightWidth       = 1;
    inline constexpr int32_t  kDistantLightSphereTexel = 0;

    // The SkyAerialPerspectiveLut pass's push block is ShaderPush::SkyAerialPerspectiveLut (generated from
    // SkyAerialPerspectiveLut.shader). Everything in it is per-VIEW and per-frame: the froxel grid is the
    // camera's own frustum, which is exactly what the shared sky payload does not carry.

    // @p towardSun must be normalized — the single normalization lives in ECS::Rules::AtmosphereSunDirection.
    inline SkyGpuPayload PackSky( const glm::vec3& towardSun, const SkySettings& sky, const SkySunInputs& sun,
                                  const AtmosphereLight1& light1 )
    {
        SkyGpuPayload payload;
        payload.SunDirection = glm::vec4( towardSun, sky.SunIntensity );
        payload.Zenith       = glm::vec4( sky.ZenithColor, sky.SkyBrightness );
        payload.Horizon      = glm::vec4( sky.HorizonColor, sky.HorizonFalloff );
        payload.Sun          = glm::vec4( sky.SunColor, sky.SunGlow );
        payload.Sunset       = glm::vec4( sky.SunsetColor, sky.SunsetIntensity );
        payload.Ground       = glm::vec4( sky.GroundColor, sky.StarIntensity );
        payload.Night        = glm::vec4( sky.NightColor, sky.SunAngularRadius );

        payload.MediumRayleigh      = glm::vec4( sky.RayleighScattering, sky.RayleighExpDistributionKm );
        payload.MediumMie           = glm::vec4( sky.MieScattering, sky.MieExpDistributionKm );
        payload.MediumMieAbsorption = glm::vec4( sky.MieAbsorption, sky.MieAnisotropy );
        payload.MediumOzone         = glm::vec4( sky.OzoneAbsorption, sky.AtmosphereHeightKm );
        payload.MediumGround        = glm::vec4( sky.GroundAlbedo, sky.MultiScatteringFactor );
        payload.MediumTentPlanet =
             glm::vec4( sky.OzoneTipAltitudeKm, sky.OzoneTipValue, sky.OzoneTentWidthKm, sky.PlanetRadius );

        payload.SkyLuminance =
             glm::vec4( sky.SkyLuminanceFactor, sky.Model == ECS::SkyModel::PhysicalAtmosphere ? 1.0f : 0.0f );
        payload.SkyApLuminance = glm::vec4( sky.SkyAndAerialPerspectiveLuminanceFactor, 0.0f );
        payload.AtmosphereLight = glm::vec4( sun.OuterSpaceIlluminance, sun.LuminanceCeiling );
        payload.SunDisk         = glm::vec4( sun.DiskColorScale, 0.0f );
        // An absent light 1 packs zeros with w = 0: the passes branch on that w and add nothing.
        payload.Light1Direction   = glm::vec4( light1.Present ? light1.TowardLight : glm::vec3( 0.0f, 1.0f, 0.0f ),
                                               light1.Present ? light1.DiskAngularRadius : 0.0f );
        payload.Light1Illuminance = glm::vec4( light1.Present ? light1.OuterSpaceIlluminance : glm::vec3( 0.0f ),
                                               light1.Present ? 1.0f : 0.0f );
        payload.Light1Disk        = glm::vec4( light1.Present ? light1.DiskColorScale : glm::vec3( 0.0f ), 0.0f );
        return payload;
    }

    // How many leading FLOATS of the packed block the bake fingerprint below skips — the sun's direction,
    // xyz of vec4 0. Named rather than spelled 3, because the test that walks the block asserts on this
    // number and a reader has to be able to see the two agree.
    inline constexpr size_t kSkyBakeFingerprintSkippedFloats = 3;

    /**
     * @brief EVERYTHING THE SKY IBL BAKE READS, AS ONE NUMBER.
     *
     * The third key of Graphic::ShouldRebakeSkyEnvironment, beside the sun's angular threshold and the
     * cloud field's fingerprint. Before it existed the environment was rebaked only when the SUN moved or
     * the CLOUDS changed, so a sky whose own parameters had been replaced — a different ground albedo, a
     * different model, a different palette, a whole different LEVEL loaded onto the same renderer — kept
     * lighting the world with the atmosphere it was baked from. The sky pixels changed immediately (they
     * are marched every frame) and the ambient did not, which reads as "the ambient is wrong" rather than
     * as "the ambient is stale".
     *
     * TAKEN OVER THE PACKED PAYLOAD, NOT OVER SkySettings, and that is the point. The bake dispatch
     * (Programs/Compute/BakeProceduralSky.shader) reads this block and nothing else of the sky, so a
     * fingerprint of the block is a fingerprint of the bake's input BY CONSTRUCTION: a vec4 appended to
     * the payload is covered the moment PackSky writes it, and SkyLayout.hpp declares the block once for
     * both sides. A hand-written field list would be a second place to
     * remember — which is exactly how AtmosphereLutFingerprint (the LUT passes' equivalent, which IS a
     * typed list because it deliberately covers only part of the block) and the bake trigger came to
     * disagree.
     *
     * THE SUN'S DIRECTION IS THE ONLY EXCLUSION. It sits in xyz of vec4 0 and it moves EVERY FRAME under
     * the time-of-day driver, so comparing it exactly would demand a rebake per frame — which is what
     * RebakeSunAngleThreshold exists to prevent. The sun is judged by ANGLE and everything else by
     * EQUALITY. The `w` of that vec4 is the sun's INTENSITY, is not a direction, has no threshold of its
     * own, and is included — as is the atmosphere sun light's illuminance (vec4 15), which the physical bake
     * integrates. The luminance ceiling in its `w` is the second exclusion: it is the VIEW's exposure, not the
     * sky, and the bake does not read it. The sky light is captured once per scene for every view
     * (SceneSkyLight), so two views with different exposures would otherwise each see "a different sky" in the
     * other's capture and recapture it on every frame.
     *
     * @param panoramaSizeKey the bake's one input that is NOT in the block — SkySettings::
     *        EnvironmentResolution, which picks the panorama's extent. Without it, changing the
     *        Environment Resolution changed nothing until the sun happened to move.
     */
    inline uint64_t SkyBakeFingerprint( const SkyGpuPayload& payload, uint32_t panoramaSizeKey )
    {
        // FNV-1a over the raw bytes. Two bit-identical blocks are the same sky; two blocks differing in any
        // bit are treated as different, which at worst costs one bake that was not needed (-0.0f against
        // 0.0f is the only way that happens and no authored path produces it).
        uint64_t hash = 1469598103934665603ull;

        const auto mixByte = [&hash]( unsigned char byte )
        {
            hash ^= static_cast<uint64_t>( byte );
            hash *= 1099511628211ull;
        };

        SkyGpuPayload bakeInput     = payload;
        bakeInput.AtmosphereLight.w = 0.0f; // the view's luminance ceiling, see above
        const auto* bytes           = reinterpret_cast<const unsigned char*>( &bakeInput );
        for ( size_t i = kSkyBakeFingerprintSkippedFloats * sizeof( float ); i < sizeof( SkyGpuPayload ); ++i )
            mixByte( bytes[i] );

        for ( size_t i = 0; i < sizeof( panoramaSizeKey ); ++i )
            mixByte( static_cast<unsigned char>( ( panoramaSizeKey >> ( i * 8 ) ) & 0xFFu ) );

        return hash;
    }
} // namespace Desert::Graphic

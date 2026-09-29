#pragma once

#include <Engine/Graphic/SkySettings.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // THE sky parameter block, byte for byte. One layout serves three consumers: the sky graphics pass, the
    // IBL bake compute dispatch.
    //
    // The GLSL side of this layout is `SkyPacked` in Engine/Content/Shaders/Common/Atmosphere.glslh, and
    // no shader reads a field of it directly — they call the unpack helpers there. That is the point: the
    // layout below can be reordered or extended without any consumer changing a line, as long as the two
    // sides move together. SKY_PACKED_VEC4_COUNT there and kSkyPackedVec4Count here are the tie, and the
    // static_asserts at the bottom make a mismatch a build error instead of a corrupted frame.
    //
    // std430 vs std140 does not matter for this block: it is an array of vec4, and both layouts agree on a
    // 16-byte stride for that.
    //
    // The block GREW from 7 to 13 vec4s when the physical-atmosphere medium landed (vec4s 7-12, read by
    // the transmittance / multi-scattering LUT passes), and to 15 when Phase 2 appended the model switch
    // and the two art-direction tints (vec4s 13-14, read by the physical sky pass, the Sky-View LUT fill
    // and the IBL bake). Extension is APPEND-ONLY: every existing offset is a promise to every shader
    // that reads the block.
    inline constexpr uint32_t kSkyPackedVec4Count = 15;

    struct SkyGpuPayload
    {
        glm::vec4 SunDirection; // 0   xyz = direction TOWARD the sun (normalized), w = sun intensity
        glm::vec4 Zenith;       // 16  rgb = zenith colour,  w = sky brightness
        glm::vec4 Horizon;      // 32  rgb = horizon colour, w = horizon falloff
        glm::vec4 Sun;          // 48  rgb = sun colour,     w = sun glow
        glm::vec4 Sunset;       // 64  rgb = sunset colour,  w = sunset intensity
        glm::vec4 Ground;       // 80  rgb = ground colour,  w = star intensity
        glm::vec4 Night;        // 96  rgb = night colour,   w = sun angular RADIUS in RADIANS

        // ---- Physical atmosphere medium (SkyModel::PhysicalAtmosphere). Coefficients are PER
        // KILOMETRE (the authored unit), altitudes and scale heights in kilometres; the one world-unit
        // quantity (the planet radius) is converted to km once, inside the shader — the same rule the
        // ----
        glm::vec4 MediumRayleigh;      // 112  rgb = Rayleigh scattering /km, w = Rayleigh scale height (km)
        glm::vec4 MediumMie;           // 128  rgb = Mie scattering /km,      w = Mie scale height (km)
        glm::vec4 MediumMieAbsorption; // 144  rgb = Mie absorption /km,      w = Mie anisotropy g
        glm::vec4 MediumOzone;         // 160  rgb = ozone absorption /km,    w = atmosphere height (km)
        glm::vec4 MediumGround;        // 176  rgb = ground albedo,           w = multi-scattering factor
        glm::vec4 MediumTentPlanet;    // 192  x = ozone tip altitude (km), y = ozone tip value,
                                       //      z = ozone tent width (km),  w = planet radius (WORLD UNITS)

        // ---- Phase 2: the model switch and the art-direction tints (appended, like the medium). ----
        glm::vec4 SkyLuminance;   // 208  rgb = Sky Luminance Factor (physical sky pixels only),
                                  //      w = sky model: 0 = ArtisticGradient, 1 = PhysicalAtmosphere
        glm::vec4 SkyApLuminance; // 224  rgb = Sky And Aerial Perspective Luminance Factor (inside
                                  //      every scattering integration), w = reserved (0)
    };

    static_assert( sizeof( SkyGpuPayload ) == kSkyPackedVec4Count * sizeof( glm::vec4 ),
                   "SkyGpuPayload must be exactly SKY_PACKED_VEC4_COUNT vec4s — the shader reads it as one." );
    static_assert( offsetof( SkyGpuPayload, SunDirection ) == 0 );
    static_assert( offsetof( SkyGpuPayload, Zenith ) == 16 );
    static_assert( offsetof( SkyGpuPayload, Horizon ) == 32 );
    static_assert( offsetof( SkyGpuPayload, Sun ) == 48 );
    static_assert( offsetof( SkyGpuPayload, Sunset ) == 64 );
    static_assert( offsetof( SkyGpuPayload, Ground ) == 80 );
    static_assert( offsetof( SkyGpuPayload, Night ) == 96 );
    static_assert( offsetof( SkyGpuPayload, MediumRayleigh ) == 112 );
    static_assert( offsetof( SkyGpuPayload, MediumMie ) == 128 );
    static_assert( offsetof( SkyGpuPayload, MediumMieAbsorption ) == 144 );
    static_assert( offsetof( SkyGpuPayload, MediumOzone ) == 160 );
    static_assert( offsetof( SkyGpuPayload, MediumGround ) == 176 );
    static_assert( offsetof( SkyGpuPayload, MediumTentPlanet ) == 192 );
    static_assert( offsetof( SkyGpuPayload, SkyLuminance ) == 208 );
    static_assert( offsetof( SkyGpuPayload, SkyApLuminance ) == 224 );

    // Size the buffer is created with, and the size every writer must hand to SetData.
    inline constexpr uint32_t kSkyPayloadBytes =
         kSkyPackedVec4Count * static_cast<uint32_t>( sizeof( glm::vec4 ) );

    // Binding number the sky parameter buffer is declared at, in BOTH sky shaders.
    //
    // It has to be one number, not two. The graphics descriptor write uses the buffer's OWN binding
    // (VulkanMaterialBackend::ApplyStorageBuffer -> VulkanStorageBuffer::GetBinding), while
    // ComputePipeline::SetStorageBuffer takes the binding as an explicit ARGUMENT. When those two disagree
    // the buffer lands on somebody else's slot — the particle system aliased the camera uniform buffer at
    // binding 0 exactly this way and tripped VUID-VkWriteDescriptorSet-descriptorType-00319. So: the buffer
    // is created with this number, both shaders declare this number, and the compute dispatch passes it.
    inline constexpr uint32_t kSkyPayloadBinding = 1;

    // The bindings the atmosphere-LUT compute passes agree on with their shaders — same explicit-argument
    // trap as kSkyPayloadBinding, same cure: one constant, both sides.
    inline constexpr uint32_t kSkyTransmittanceLutOutputBinding = 0; // SkyTransmittanceLut: the image it fills
    inline constexpr uint32_t kSkyMultiScatterLutOutputBinding  = 0; // SkyMultiScatterLut: the image it fills
    inline constexpr uint32_t kSkyViewLutOutputBinding          = 0; // SkyViewLut: the image it fills
    inline constexpr uint32_t kSkyAerialPerspectiveOutputBinding = 0; // SkyAerialPerspectiveLut: the volume
    inline constexpr uint32_t kSkyDistantLightOutputBinding      = 0; // SkyDistantLight: the texel it fills
    // LUT INPUT bindings, shared by every compute consumer (SkyMultiScatterLut reads the transmittance
    // at 2; SkyViewLut and BakeProceduralSky read the transmittance at 2 and the multi-scatter at 3).
    inline constexpr uint32_t kSkyTransmittanceLutBinding = 2;
    inline constexpr uint32_t kSkyMultiScatterLutBinding  = 3;

    // The Sky-View LUT extent — Hillaire's 192x104, mirrored by SKY_VIEW_LUT_WIDTH/HEIGHT in
    // Common/SkyScattering.glslh (the sub-texel remap bakes the numbers into every read and write, so
    // the two sides must state the same extent; the SkyScattering tests pin the shader-side pair).
    inline constexpr uint32_t kSkyViewLutWidth  = 192;
    inline constexpr uint32_t kSkyViewLutHeight = 104;

    // Push block of the SkyViewLut pass — mirrored by `PushConstant SkyViewPush` in SkyViewLut.shader.
    // The LUT depends on the camera (its altitude picks the horizon warp), and the camera is the one
    // per-frame quantity the shared payload deliberately does not carry.
    struct SkyViewLutPush
    {
        glm::vec4 CameraPosWorld; // xyz = camera position in WORLD UNITS (centimetres), w unused
    };
    static_assert( sizeof( SkyViewLutPush ) == 16 );

    // The camera aerial-perspective volume's extent — Hillaire's 32x32x16, mirrored by
    // SKY_AP_VOLUME_WIDTH/HEIGHT/DEPTH in Common/SkyScattering.glslh. Both sides must state the same
    // three numbers: the slice mapping's texel-centre remap bakes the depth into every write and every
    // read, and the x/y remap does the same for the screen extent.
    inline constexpr uint32_t kAerialPerspectiveWidth  = 32;
    inline constexpr uint32_t kAerialPerspectiveHeight = 32;
    inline constexpr uint32_t kAerialPerspectiveDepth  = 16;

    // The distant sky light's extent, and the texel it holds. ONE march of 64 directions reduced once
    // (Programs/Sky/SkyDistantLight.shader):
    //   x = 0  the FULL-SPHERE mean — UE's Distant Sky Light, read by the height fog, which is lit from
    //          every direction at once and has no ground term of its own.
    // The shader indexes this by number, so the number lives here and not in a shader comment.
    inline constexpr uint32_t kDistantLightWidth       = 1;
    inline constexpr int32_t  kDistantLightSphereTexel = 0;

    // Push block of the SkyAerialPerspectiveLut pass — mirrored by `PushConstant SkyApPush` in
    // SkyAerialPerspectiveLut.shader. Everything here is per-VIEW and per-frame: the froxel grid is the
    // camera's own frustum, which is exactly what the shared sky payload does not carry.
    struct SkyAerialPerspectivePush
    {
        glm::mat4 InverseViewProjection;
        glm::vec4 CameraPosWorld; // xyz = camera position in WORLD UNITS (centimetres), w unused
        glm::vec4 VolumeParams;   // x = volume depth (km), y = start depth (km), z/w reserved (0)
    };
    static_assert( sizeof( SkyAerialPerspectivePush ) == 96 );

    // @p towardSun must be normalized — the single normalization lives in ECS::Rules::AtmosphereSunDirection.
    inline SkyGpuPayload PackSky( const glm::vec3& towardSun, const SkySettings& sky )
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
     * the payload is covered the moment PackSky writes it, and the static_assert above ties the block's
     * size to the shader's own SKY_PACKED_VEC4_COUNT. A hand-written field list would be a second place to
     * remember — which is exactly how AtmosphereLutFingerprint (the LUT passes' equivalent, which IS a
     * typed list because it deliberately covers only part of the block) and the bake trigger came to
     * disagree.
     *
     * THE SUN'S DIRECTION IS THE ONLY EXCLUSION. It sits in xyz of vec4 0 and it moves EVERY FRAME under
     * the time-of-day driver, so comparing it exactly would demand a rebake per frame — which is what
     * RebakeSunAngleThreshold exists to prevent. The sun is judged by ANGLE and everything else by
     * EQUALITY. The `w` of that vec4 is the sun's INTENSITY, is not a direction, has no threshold of its
     * own, and is included.
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

        const auto* bytes = reinterpret_cast<const unsigned char*>( &payload );
        for ( size_t i = kSkyBakeFingerprintSkippedFloats * sizeof( float ); i < sizeof( SkyGpuPayload ); ++i )
            mixByte( bytes[i] );

        for ( size_t i = 0; i < sizeof( panoramaSizeKey ); ++i )
            mixByte( static_cast<unsigned char>( ( panoramaSizeKey >> ( i * 8 ) ) & 0xFFu ) );

        return hash;
    }
} // namespace Desert::Graphic

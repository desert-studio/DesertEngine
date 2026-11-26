#pragma once

// THE SKY PARAMETER BLOCK, declared once: Tools/ShaderProtocolGen writes the GLSL struct into
// Generated/Sky/SkyPacked.glslh (included by Common/Atmosphere.glslh, whose unpack helpers are the only readers of
// its fields) and the buffer every sky pass binds into Generated/Sky/SkyBuffer.glslh, and checks the std430 layout
// against these declarations at compile time.
//
// Extension is APPEND-ONLY: every existing offset is a promise to every shader that reads the block. The block
// grew from 7 to 13 vec4s when the physical-atmosphere medium landed, to 15 with the model switch and the two
// art-direction tints, to 17 with the atmosphere sun light, and to 20 with atmosphere light 1.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::Graphic
{
    // THE sky parameter block (GLSL SkyPacked), filled by PackSky. Coefficients of the physical medium are PER
    // KILOMETRE (the authored unit), altitudes and scale heights in kilometres; the one world-unit quantity (the
    // planet radius) is converted to km once, inside the shader.
#define DESERT_SKY_GPU_PAYLOAD_FIELDS( X )                                                                        \
    X( glm::vec4, SunDirection, "SunDirection", {} ) /* xyz = TOWARD the sun (normalized), w = sun intensity */   \
    X( glm::vec4, Zenith, "Zenith", {} )             /* rgb = zenith colour,  w = sky brightness */               \
    X( glm::vec4, Horizon, "Horizon", {} )           /* rgb = horizon colour, w = horizon falloff */              \
    X( glm::vec4, Sun, "Sun", {} )                   /* rgb = sun colour,     w = sun glow */                     \
    X( glm::vec4, Sunset, "Sunset", {} )             /* rgb = sunset colour,  w = sunset intensity */             \
    X( glm::vec4, Ground, "Ground", {} )             /* rgb = ground colour,  w = star intensity */               \
    X( glm::vec4, Night, "Night", {} )               /* rgb = night colour,   w = sun angular RADIUS (rad) */     \
    /* ---- Physical atmosphere medium (SkyModel::PhysicalAtmosphere). ---- */                                    \
    X( glm::vec4, MediumRayleigh, "MediumRayleigh", {} )           /* rgb = Rayleigh /km, w = scale height km */  \
    X( glm::vec4, MediumMie, "MediumMie", {} )                     /* rgb = Mie /km, w = Mie scale height km */   \
    X( glm::vec4, MediumMieAbsorption, "MediumMieAbsorption", {} ) /* rgb = Mie absorption /km, w = g */          \
    X( glm::vec4, MediumOzone, "MediumOzone", {} )   /* rgb = ozone absorption /km, w = atmosphere height (km) */ \
    X( glm::vec4, MediumGround, "MediumGround", {} ) /* rgb = ground albedo, w = multi-scattering factor */       \
    /* x = ozone tip altitude (km), y = ozone tip value, z = ozone tent width (km), w = planet radius (WORLD */   \
    /* UNITS) */                                                                                                  \
    X( glm::vec4, MediumTentPlanet, "MediumTentPlanet", {} )                                                      \
    /* ---- The model switch and the art-direction tints. ---- */                                                 \
    /* rgb = Sky Luminance Factor (physical sky pixels only), w = 0 ArtisticGradient, 1 PhysicalAtmosphere */     \
    X( glm::vec4, SkyLuminance, "SkyLuminance", {} )                                                              \
    /* rgb = Sky And Aerial Perspective Luminance Factor (inside every scattering integration), w = 0 */          \
    X( glm::vec4, SkyApLuminance, "SkyApLuminance", {} )                                                          \
    /* ---- The atmosphere sun light. PhysicalAtmosphere reads these; the gradient does not. ---- */              \
    /* rgb = outer-space illuminance (lux, Color x Intensity), w = on-screen sky luminance ceiling */             \
    X( glm::vec4, AtmosphereLight, "AtmosphereLight", {} )                                                        \
    X( glm::vec4, SunDisk, "SunDisk", {} ) /* rgb = Atmosphere Sun Disk Color Scale, w = 0 */                     \
    /* ---- Atmosphere light 1 (UE's Atmosphere Sun Light Index 1, the moon). PhysicalAtmosphere only. ---- */    \
    X( glm::vec4, Light1Direction, "Light1Direction", {} ) /* xyz = TOWARD light 1, w = disc angular RADIUS */    \
    X( glm::vec4, Light1Illuminance, "Light1Illuminance", {} ) /* rgb = lux, w = 1 when the light exists */       \
    X( glm::vec4, Light1Disk, "Light1Disk", {} )               /* rgb = its Sun Disk Color Scale, w = 0 */
    DESERT_SHADER_STRUCT( SkyGpuPayload, "SkyPacked", Std430, DESERT_SKY_GPU_PAYLOAD_FIELDS );
#undef DESERT_SKY_GPU_PAYLOAD_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSkyPackedStructs[] = { &kSkyGpuPayloadStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kSkyPackedShaderInclude{
               .Path         = "Generated/Sky/SkyPacked.glslh",
               .SourceHeader = "Desert/Desert/Source/Engine/Graphic/SkyLayout.hpp",
               .Structs      = kSkyPackedStructs,
    };

    // Binding number the sky parameter buffer is declared at, in EVERY sky pass (the generated SkyBuffer).
    //
    // It has to be one number. The graphics descriptor write uses the buffer's OWN binding
    // (VulkanMaterialBackend::ApplyStorageBuffer -> VulkanStorageBuffer::GetBinding), while
    // ComputePipeline::SetStorageBuffer takes the binding as an explicit ARGUMENT. When those two disagree the
    // buffer lands on somebody else's slot — the particle system aliased the camera uniform buffer at binding 0
    // exactly this way and tripped VUID-VkWriteDescriptorSet-descriptorType-00319. So: the buffer is created with
    // this number, the generated block declares this number, and the compute dispatch passes it.
    inline constexpr uint32_t kSkyPayloadBinding = 1;

    // The buffer the sky passes bind: the whole payload as one member, so a pass copies it into its SkyPacked in
    // one assignment (`SkyPacked s = SkyPackedData;`).
#define DESERT_SKY_BUFFER_GPU_FIELDS( X ) X( SkyGpuPayload, Payload, "SkyPackedData", {} )
    DESERT_SHADER_STRUCT( SkyBufferGpu, "SkyBufferGpu", Std430, DESERT_SKY_BUFFER_GPU_FIELDS );
#undef DESERT_SKY_BUFFER_GPU_FIELDS

    inline constexpr std::string_view                   kSkyBufferRequires[] = { "Generated/Sky/SkyPacked.glslh" };
    inline constexpr ShaderProtocols::ShaderBlockDecl   kSkyBufferBlocks[]   = { {
             .Kind      = ShaderProtocols::ShaderBlockKind::ReadBuffer,
             .Binding   = { ShaderProtocols::ShaderBindingKind::Fixed, kSkyPayloadBinding, {} },
             .BlockName = "SkyBuffer",
             .Members   = &kSkyBufferGpuStruct,
    } };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kSkyBufferShaderInclude{
         .Path         = "Generated/Sky/SkyBuffer.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/SkyLayout.hpp",
         .Requires     = kSkyBufferRequires,
         .Blocks       = kSkyBufferBlocks,
    };
} // namespace Desert::Graphic

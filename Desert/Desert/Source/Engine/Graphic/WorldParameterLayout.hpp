#pragma once

// THE WORLD PARAMETER TABLE'S GPU RECORDS, declared once: the table's header and one open-addressed slot.
// Tools/ShaderProtocolGen writes them as Generated/WorldParameters.glslh, which Common/WorldParameters.glslh
// includes; its block is a header, then `Capacity` slots (WorldParameterTable.hpp BuildWorldParameterTable).

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic
{
#define DESERT_GPU_WORLD_PARAMETER_HEADER_FIELDS( X )                                                             \
    X( uint32_t, Capacity, "Capacity", 1u ) /* a power of two */                                                  \
    X( uint32_t, Count, "Count", 0u )       /* the parameter count */                                             \
    X( uint32_t, Pad0, "Pad0", 0u )                                                                               \
    X( uint32_t, Pad1, "Pad1", 0u )
    DESERT_SHADER_STRUCT( GpuWorldParameterHeader, "WorldParameterHeader", Std430,
                          DESERT_GPU_WORLD_PARAMETER_HEADER_FIELDS );
#undef DESERT_GPU_WORLD_PARAMETER_HEADER_FIELDS

#define DESERT_GPU_WORLD_PARAMETER_SLOT_FIELDS( X )                                                               \
    X( uint32_t, Key, "Key", 0u ) /* 0 = empty */                                                                 \
    X( uint32_t, Pad0, "Pad0", 0u )                                                                               \
    X( uint32_t, Pad1, "Pad1", 0u )                                                                               \
    X( uint32_t, Pad2, "Pad2", 0u )                                                                               \
    X( glm::vec4, Value, "Value", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( GpuWorldParameterSlot, "WorldParameterSlot", Std430,
                          DESERT_GPU_WORLD_PARAMETER_SLOT_FIELDS );
#undef DESERT_GPU_WORLD_PARAMETER_SLOT_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kWorldParameterStructs[] = {
         &kGpuWorldParameterHeaderStruct,
         &kGpuWorldParameterSlotStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kWorldParameterShaderInclude{
         .Path         = "Generated/WorldParameters.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/WorldParameterLayout.hpp",
         .Structs      = kWorldParameterStructs,
    };
} // namespace Desert::Graphic

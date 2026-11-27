#pragma once

// THE NEURAL RADIANCE CACHE'S GPU RECORDS, declared once: a path vertex, a query of the image, a training sample
// and the trace's parameters. Tools/ShaderProtocolGen writes them as Generated/Neural/NeuralRadianceCache.glslh,
// which Common/NeuralRadianceCache.glslh includes (PathTracer.shader's PATH_TRACING_NRC variant through it).

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Engine::Neural
{
    // A path vertex: A.xyz = floatBitsToUint(world position cm), A.w = packUnorm2x16(octahedral(w_o) * 0.5 + 0.5);
    // B.x = packUnorm2x16(octahedral(normal) * 0.5 + 0.5), B.y = packUnorm2x16(vec2(roughness, 0)),
    // B.z = packUnorm4x8(vec4(D_d, 0)), B.w = packUnorm4x8(vec4(D_s, 0)).
#define DESERT_NRC_VERTEX_GPU_FIELDS( X )                                                                         \
    X( glm::uvec4, A, "A", glm::uvec4( 0u ) )                                                                     \
    X( glm::uvec4, B, "B", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( NrcVertexGpu, "NrcVertex", Std430, DESERT_NRC_VERTEX_GPU_FIELDS );
#undef DESERT_NRC_VERTEX_GPU_FIELDS

    // One path of the image that ended in the cache, appended to its path's segment (halves packed by
    // packHalf2x16, saturated at 65504): the vertex; Lobes.xy = DiffuseWeight (rgb T * (1 - share) / D_d1, a
    // lum(I_d) of the path before the cache), Lobes.zw = SpecularWeight (rgb T * share / D_s1, a lum(I_s) before
    // the cache) (share = the first surface's bounce specular share, D_x1 its guides:
    // Common/PathTracingDenoise.glslh; both 0 when the path had no first-surface bounce); Weight.xy = T (0: a path
    // dropped after its claim adds nothing), Weight.z = the pixel y * width + x, Weight.w = 0.
#define DESERT_NRC_QUERY_GPU_FIELDS( X )                                                                          \
    X( NrcVertexGpu, Vertex, "Vertex", {} )                                                                       \
    X( glm::uvec4, Lobes, "Lobes", glm::uvec4( 0u ) )                                                             \
    X( glm::uvec4, Weight, "Weight", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( NrcQueryGpu, "NrcQuery", Std430, DESERT_NRC_QUERY_GPU_FIELDS );
#undef DESERT_NRC_QUERY_GPU_FIELDS

    // A training sample: the vertex, Radiance.rgb = (R_end - R_i) / T_i, Radiance.a = uintBitsToFloat(the self
    // query index, NRC_NO_SELF_QUERY none), Throughput.rgb = T_end / T_i.
#define DESERT_NRC_TRAINING_SAMPLE_GPU_FIELDS( X )                                                                \
    X( NrcVertexGpu, Vertex, "Vertex", {} )                                                                       \
    X( glm::vec4, Radiance, "Radiance", glm::vec4( 0.0f ) )                                                       \
    X( glm::vec4, Throughput, "Throughput", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( NrcTrainingSampleGpu, "NrcTrainingSample", Std430,
                          DESERT_NRC_TRAINING_SAMPLE_GPU_FIELDS );
#undef DESERT_NRC_TRAINING_SAMPLE_GPU_FIELDS

    // The trace's NRC parameters: the contents of NrcTraceTargets::Params, read by the PATH_TRACING_NRC trace as
    // PathTracer.shader NrcTraceParamsUB. A buffer, not the trace's push: the push is full (ShaderPush::PathTracer
    // is Vulkan's guaranteed 128 bytes with the light grid's descriptor).
#define DESERT_NRC_TRACE_PARAMS_FIELDS( X )                                                                       \
    /* x = each segment's query capacity (> 0: Nrc), y = training tile side, z / w = this frame's pixel offset in \
       the tile */                                                                                                \
    X( glm::uvec4, Params, "NrcParams", glm::uvec4( 0u ) )                                                        \
    /* x = kNrcSpreadThreshold, y = kNrcUnbiasedTrainingPaths, z = kNrcMinThroughput, w = 0 */                    \
    X( glm::vec4, Spread, "NrcSpread", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( NrcTraceParams, "NrcTraceParams", Std430, DESERT_NRC_TRACE_PARAMS_FIELDS );
#undef DESERT_NRC_TRACE_PARAMS_FIELDS

    inline constexpr const Graphic::ShaderProtocols::ShaderStructDecl* kNeuralRadianceCacheStructs[] = {
         &kNrcVertexGpuStruct,
         &kNrcQueryGpuStruct,
         &kNrcTrainingSampleGpuStruct,
         &kNrcTraceParamsStruct,
    };
    inline constexpr Graphic::ShaderProtocols::ShaderIncludeDecl kNeuralRadianceCacheShaderInclude{
         .Path         = "Generated/Neural/NeuralRadianceCache.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Neural/NeuralRadianceCacheLayout.hpp",
         .Structs      = kNeuralRadianceCacheStructs,
    };
} // namespace Desert::Engine::Neural

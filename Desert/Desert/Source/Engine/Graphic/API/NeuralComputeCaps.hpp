#pragma once

#include <Common/Settings/NeuralKernelPath.hpp>

#include <cstdint>
#include <string>
#include <vector>

// WHAT THE DEVICE OFFERS A NEURAL-NETWORK KERNEL, AND WHICH KERNEL PATH THAT SELECTS (NN-RUNTIME step 1).
//
// The engine's NN layer is its own, on Vulkan compute (decision 10-28, Wiki «NN-RUNTIME · Слой запуска
// нейросетей»): fully-fused MLP kernels, weights in shared memory, one dispatch per network. The same math has
// three implementations, chosen once per device from this struct:
//   * CoopMatFp16 — VK_KHR_cooperative_matrix tiles (tensor cores / WMMA / XMX) on fp16 inputs;
//   * Fp16        — scalar fp16 arithmetic and fp16 weights in storage buffers;
//   * Fp32        — the base path every Vulkan device runs.
// The three are equal implementations of one network (step 2 compares their frames within a tolerance), so the
// choice is a selection, not a fallback: it is made here, once, with the reason a faster path was not taken, and
// logged at start-up ("[nn_caps] ...").
//
// Backend-neutral on purpose: no Vulkan types. The Vulkan probe (DeviceCapsProbe.cpp) fills it; Device.hpp's
// DeviceCapabilities carries it to the NN layer and to the editor's control channel.
namespace Desert::Engine
{
    // A cooperative-matrix component type (VkComponentTypeKHR, the subset a network can use; anything else is
    // recorded as Other so a combination is never dropped from the census).
    enum class NeuralScalar : uint8_t
    {
        Float16,
        Float32,
        Float64,
        BFloat16,
        Sint8,
        Uint8,
        Sint32,
        Uint32,
        Other,
    };

    // One VkCooperativeMatrixPropertiesKHR row: C(MxN) = A(MxK) * B(KxN) + C, as the driver lists it.
    struct CooperativeMatrixShape
    {
        uint32_t     M                      = 0;
        uint32_t     N                      = 0;
        uint32_t     K                      = 0;
        NeuralScalar A                      = NeuralScalar::Other;
        NeuralScalar B                      = NeuralScalar::Other;
        NeuralScalar C                      = NeuralScalar::Other;
        NeuralScalar Result                 = NeuralScalar::Other;
        bool         SaturatingAccumulation = false;
        bool         SubgroupScope          = false; ///< VK_SCOPE_SUBGROUP_KHR (the only scope the kernels use)
    };

    // Declared in Common (the machine's forced path is written by these names, MachineSettings).
    using NeuralKernelPath = ::Common::Settings::NeuralKernelPath;

    struct NeuralComputeCaps
    {
        // Device features, each true only when present AND enabled on the logical device (DeviceCaps rows).
        bool ShaderFloat16            = false; ///< shaderFloat16: fp16 arithmetic in shaders
        bool ShaderInt8               = false; ///< shaderInt8: int8 arithmetic in shaders
        bool StorageBuffer16BitAccess = false; ///< fp16 weights / activations in storage buffers
        bool StorageBuffer8BitAccess  = false; ///< int8 weights in storage buffers
        bool SubgroupSizeControl      = false; ///< subgroupSizeControl + computeFullSubgroups
        bool CooperativeMatrix        = false; ///< VK_KHR_cooperative_matrix, feature cooperativeMatrix
        bool VulkanMemoryModel        = false; ///< vulkanMemoryModel: every coopmat module glslang emits uses it

        // Device limits (VkPhysicalDeviceLimits / subgroup properties).
        uint32_t SubgroupSize                  = 0;     ///< VkPhysicalDeviceSubgroupProperties::subgroupSize
        uint32_t MinSubgroupSize               = 0;     ///< with SubgroupSizeControl only; 0 without it
        uint32_t MaxSubgroupSize               = 0;     ///< with SubgroupSizeControl only; 0 without it
        bool     ComputeSubgroupSizeRequirable = false; ///< requiredSubgroupSizeStages has COMPUTE
        uint32_t MaxPushConstantsSize          = 0;
        uint32_t MaxComputeSharedMemorySize    = 0;

        // Cooperative matrix: whether the compute stage may use it, and every combination the driver lists.
        bool                                CooperativeMatrixInCompute = false;
        std::vector<CooperativeMatrixShape> CooperativeMatrixShapes;

        // The selection (SelectNeuralKernelPath): the path, the coopmat tile it uses (CoopMatFp16 only), and why
        // each faster path was not taken (empty when the fastest one was).
        NeuralKernelPath       Path = NeuralKernelPath::Fp32;
        CooperativeMatrixShape Tile{};
        std::string            Reason;
    };

    [[nodiscard]] const char* NeuralKernelPathName( NeuralKernelPath path );
    [[nodiscard]] const char* NeuralScalarName( NeuralScalar scalar );

    // PURE. Sets Path, Tile and Reason from the fields above:
    //   CoopMatFp16 iff CooperativeMatrix, VulkanMemoryModel, CooperativeMatrixInCompute, ShaderFloat16,
    //               StorageBuffer16BitAccess and
    //   a
    //               subgroup-scope shape with A = B = fp16 and C = Result = fp32 or fp16 (fp32 accumulation
    //               preferred, then the largest M*N*K, then the smallest K);
    //   Fp16        iff ShaderFloat16 and StorageBuffer16BitAccess;
    //   Fp32        otherwise.
    // Reason names every missing piece of every faster path, not the first.
    void SelectNeuralKernelPath( NeuralComputeCaps& caps );

    // "16x16x16 f16*f16+f32->f32" (" sat" / " non-subgroup" appended when they apply).
    [[nodiscard]] std::string FormatCooperativeMatrixShape( const CooperativeMatrixShape& shape );

    // The start-up log line: "[nn_caps] path=... (reason) | every field = its value | coopmat shapes".
    [[nodiscard]] std::string FormatNeuralComputeCaps( const NeuralComputeCaps& caps );
} // namespace Desert::Engine

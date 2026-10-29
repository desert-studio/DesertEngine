#pragma once

#include <cstdint>

namespace Common::Settings
{
    // THE NN LAYER'S KERNEL PATHS (NN-RUNTIME; Engine/Graphic/API/NeuralComputeCaps.hpp, Engine/Graphic/Neural/).
    // One declaration for both readers: the device selection (SelectNeuralKernelPath, Engine) and the machine's
    // forced path (MachineSettings::ForcedNeuralKernelPath, written by name: "Fp32", "Fp16", "CoopMatFp16").
    enum class NeuralKernelPath : int
    {
        Fp32,        ///< scalar fp32 — the base path every Vulkan device runs
        Fp16,        ///< scalar fp16 arithmetic, fp16 activations in storage and shared memory
        CoopMatFp16, ///< VK_KHR_cooperative_matrix tiles on fp16 inputs (tensor cores / WMMA / XMX)
    };
} // namespace Common::Settings

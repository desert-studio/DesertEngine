#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Neural/FusedMlp.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Graphic
{
    class ComputePipeline;
}

// THE FULLY FUSED MLP ON THE GPU (NN-RUNTIME step 3): the kernel path this machine runs, the pipelines of
// Shaders/Programs/Compute/NeuralFusedMlp.shader per kernel x path x tile, and one forward + backward + weight
// gradient of a FusedMlpProblem, read back for the CPU reference to judge (CheckFusedMlpPath, DesertCtl nn-check).
//
// Immediate dispatches (each one waits on its fence): this is the check and the bring-up runner. Running a network
// inside a frame is a render-graph compute pass, which the first network that runs per frame (NRC) adds.
namespace Desert::Engine::Neural
{
    // THE PATH THE NN LAYER RUNS on this device. Absent @p forced = the device's selection (caps.Path). A forced
    // path the device cannot run is REFUSED naming what it lacks, never replaced by another path: fp16 needs
    // shaderFloat16 + storageBuffer16BitAccess, coopmat_fp16 needs the device to have selected it (caps.Reason
    // names every missing piece and caps.Tile is the tile it runs). Callers pass
    // Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath.
    [[nodiscard]] Common::ResultStr<NeuralKernelPath>
    ResolveNeuralKernelPath( const NeuralComputeCaps& caps, std::optional<NeuralKernelPath> forced );

    // Runs FusedMlpProblems on one device. Holds the pipelines it built (one per kernel x path; the coopmat ones
    // for caps.Tile) for its lifetime; the buffers of a run live for that run.
    class FusedMlpCompute
    {
    public:
        explicit FusedMlpCompute( const NeuralComputeCaps& caps );
        ~FusedMlpCompute();

        FusedMlpCompute( const FusedMlpCompute& )            = delete;
        FusedMlpCompute& operator=( const FusedMlpCompute& ) = delete;

        // Forward (outputs + every h_l), backward (every g_l) and weight gradient of @p problem on @p path, read
        // back: Output, WeightGrad and Activations (h_l as fp32, whatever the path stores them in). Refuses a
        // path ResolveNeuralKernelPath refuses, a problem ValidateFusedMlp refuses, a coopmat tile that does not
        // divide the width, a pipeline that does not build, and a buffer that cannot be written or read.
        [[nodiscard]] Common::ResultStr<FusedMlpResult> Run( const FusedMlpProblem& problem,
                                                             NeuralKernelPath       path );

    private:
        [[nodiscard]] Common::ResultStr<std::shared_ptr<Graphic::ComputePipeline>>
        Pipeline( uint32_t kernel, NeuralKernelPath path );

        NeuralComputeCaps                                                m_Caps;
        std::map<std::string, std::shared_ptr<Graphic::ComputePipeline>> m_Pipelines; ///< by the variant's defines
    };

    // THE nn-check PROBLEM: shape 32 -> 5 x 64 -> 16, batch 1024, one fixed seed ("NN-R"), so every machine and
    // every path is measured on the same bytes.
    inline constexpr FusedMlpShape kFusedMlpCheckShape{ .InputDims = 32, .OutputDims = 16, .HiddenLayers = 5 };
    inline constexpr uint32_t      kFusedMlpCheckBatch = 1024;
    inline constexpr uint64_t      kFusedMlpCheckSeed  = 0x4E4E2D52;

    // One path measured against the CPU reference (of the fp16-rounded problem for the fp16 paths).
    struct FusedMlpPathCheck
    {
        NeuralKernelPath Path = NeuralKernelPath::Fp32;
        std::string      Tile;              ///< FormatCooperativeMatrixShape for coopmat_fp16, empty otherwise
        FusedMlpError    Output;            ///< the network outputs
        FusedMlpError    WeightGrad;        ///< dL/dW of every matrix
        uint64_t         MaskFlips = 0;     ///< hidden units whose ReLU mask differs from the reference's
        double           Tolerance = 0;     ///< FusedMlpTolerance( Path )
        bool             Pass      = false; ///< both relative errors within Tolerance
    };

    // Runs the nn-check problem on @p path with @p compute and compares it. Refusals are Run's.
    [[nodiscard]] Common::ResultStr<FusedMlpPathCheck>
    CheckFusedMlpPath( FusedMlpCompute& compute, const NeuralComputeCaps& caps, NeuralKernelPath path );
} // namespace Desert::Engine::Neural

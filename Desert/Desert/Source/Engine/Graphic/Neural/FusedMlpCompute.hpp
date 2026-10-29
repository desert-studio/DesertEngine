#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Neural/FusedMlp.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Graphic
{
    class ComputePipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::RDG
{
    class Builder;
} // namespace Desert::Graphic::RDG

// THE FULLY FUSED MLP ON THE GPU (NN-RUNTIME steps 3-4): the kernel path this machine runs, the pipelines of
// Shaders/Programs/Compute/NeuralFusedMlp.shader per kernel x path x tile, and one training step of a network
// (forward, backward, weight gradient) as three render-graph compute passes. The graph places every barrier from
// the passes' declared reads and writes (forward -> backward -> weight gradient are compute -> compute
// dependencies on the activations and gradients) and, for a read-back, the compute -> host barrier and the
// invalidation of non-coherent memory (Access::HostRead extraction). The one entry point is AddTrainingPasses:
// nn-check (FusedMlpCheckRun) and the network that runs per frame (NRC) both call it.
namespace Desert::Engine::Neural
{
    // THE PATH THE NN LAYER RUNS on this device. Absent @p forced = the device's selection (caps.Path). A forced
    // path the device cannot run is REFUSED naming what it lacks, never replaced by another path: fp16 needs
    // shaderFloat16 + storageBuffer16BitAccess, coopmat_fp16 needs the device to have selected it (caps.Reason
    // names every missing piece and caps.Tile is the tile it runs). Callers pass
    // Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath.
    [[nodiscard]] Common::ResultStr<NeuralKernelPath>
    ResolveNeuralKernelPath( const NeuralComputeCaps& caps, std::optional<NeuralKernelPath> forced );

    // The subgroup control the coopmat kernels' pipelines are created with (ComputePipelineSpecification): the
    // tile loop hands a workgroup's tiles to its subgroups and every cooperative-matrix operation needs every lane
    // of its subgroup, so the subgroups must be full; and the driver lists its coopmat shapes for its
    // subgroupSize, so a device whose subgroup width varies (AMD wave32 / wave64, Intel 8..32) is held to that
    // width. A device with a fixed width (min == max, NVIDIA 32) needs only the full-subgroup flag.
    struct NeuralSubgroupRequest
    {
        uint32_t    RequiredSubgroupSize = 0;     ///< 0 = the driver's (fixed) width
        bool        RequireFullSubgroups = false; ///< VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT
        std::string Reason;                       ///< why, logged with the pipeline ("[nn_pipeline] ...")
    };

    // Refused when the device's width does not divide the coopmat workgroup (kFusedMlpCoopMatThreads) or the
    // width varies and the device cannot require one for compute (requiredSubgroupSizeStages lacks COMPUTE).
    [[nodiscard]] Common::ResultStr<NeuralSubgroupRequest> CoopMatSubgroupRequest( const NeuralComputeCaps& caps );

    // Invocations of one coopmat workgroup (NeuralFusedMlp.shader NN_THREADS for NN_PATH 2).
    inline constexpr uint32_t kFusedMlpCoopMatThreads = 128;

    // The caller's buffers of one training step, all fp32 in the layouts of FusedMlp.hpp: Weights
    // [MatrixCount][64][64], Input and OutputGrad [Batch][64]. Each must have been written by an earlier pass of
    // the same graph (an upload, or the producer's pass): the graph orders the three passes after it.
    struct FusedMlpGraphInputs
    {
        Graphic::RDG::BufferRef Weights;
        Graphic::RDG::BufferRef Input;
        Graphic::RDG::BufferRef OutputGrad;
        FusedMlpShape           Shape;
        uint32_t                Batch = 0;
    };

    // The transients the three passes write: Output [Batch][64] fp32, Activations / Grads [MatrixCount][Batch][64]
    // (fp32 on the fp32 path, fp16 on the fp16 paths: FusedMlpStoredElementBytes), WeightGrad [MatrixCount][64][64]
    // fp32. A consumer reads them in later passes of the same graph, or extracts them.
    struct FusedMlpGraphOutputs
    {
        Graphic::RDG::BufferRef Output;
        Graphic::RDG::BufferRef Activations;
        Graphic::RDG::BufferRef Grads;
        Graphic::RDG::BufferRef WeightGrad;
    };

    // Bytes of one activation / gradient element on @p path: the fp16 paths store h_l and g_l as fp16.
    [[nodiscard]] uint32_t FusedMlpStoredElementBytes( NeuralKernelPath path );

    // Builds the pipelines of one device (one per kernel x path; the coopmat ones for caps.Tile, with the
    // subgroup control CoopMatSubgroupRequest gives) and adds the passes that run them.
    class FusedMlpCompute
    {
    public:
        explicit FusedMlpCompute( const NeuralComputeCaps& caps );
        ~FusedMlpCompute();

        FusedMlpCompute( const FusedMlpCompute& )            = delete;
        FusedMlpCompute& operator=( const FusedMlpCompute& ) = delete;

        // THE TRAINING STEP AS GRAPH PASSES: "NN: Forward" (reads Weights, Input; writes Output, Activations),
        // "NN: Backward" (reads Weights, OutputGrad, Activations; writes Grads), "NN: Weight gradient" (reads
        // Activations, Grads; writes WeightGrad), each a compute pass whose binding block is its declaration.
        // Refuses (adding nothing) a path ResolveNeuralKernelPath refuses, a shape / batch ValidateFusedMlp
        // refuses, an invalid input handle, a coopmat tile that does not divide the width and a pipeline that
        // does not build.
        [[nodiscard]] Common::ResultStr<FusedMlpGraphOutputs>
        AddTrainingPasses( Graphic::RDG::Builder& graph, const FusedMlpGraphInputs& inputs, NeuralKernelPath path );

    private:
        struct Kernel
        {
            std::shared_ptr<Graphic::ComputePipeline> Pipeline;
            Graphic::ShaderBindingLayoutCache          Layout; ///< the reflected slots its binding block names
        };

        // The pipeline of @p kernel on @p path, built on first use and kept for this object's lifetime.
        [[nodiscard]] Common::ResultStr<Kernel*> AcquireKernel( uint32_t kernel, NeuralKernelPath path );

        NeuralComputeCaps             m_Caps;
        std::map<std::string, Kernel> m_Kernels; ///< by the variant's defines
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
        double           Tolerance         = 0;     ///< FusedMlpTolerance( Path ), for Output
        double           GradientTolerance = 0;     ///< FusedMlpGradientTolerance( Path, Batch ), for WeightGrad
        bool             Pass              = false; ///< each relative error within its tolerance
    };

    // ONE nn-check RUN IN FLIGHT: the check problem uploaded into a graph of its own ("NeuralFusedMlpCheck"),
    // AddTrainingPasses on @p path, Output / WeightGrad / Activations extracted with Access::HostRead, executed
    // into the frame being recorded (Renderer::ExecuteGraph: the caller is between BeginFrame and EndFrame). The
    // read-backs hold that frame's bytes once its fence has been waited, which EndFrame does for a slot before the
    // slot is reused: IsComplete says when, Complete reads and judges them.
    class FusedMlpCheckRun
    {
    public:
        // Refuses what AddTrainingPasses refuses, a graph that did not execute and a pass it faulted (the
        // faults named), and a read-back the graph handed back without memory.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<FusedMlpCheckRun>>
        Submit( FusedMlpCompute& compute, const NeuralComputeCaps& caps, NeuralKernelPath path );

        // The frame that recorded the run has completed on the GPU (FrameManager: MaxFramesInFlight frames
        // have ended since, so EndFrame has waited its slot's fence).
        [[nodiscard]] bool IsComplete() const;

        // The GPU result against the CPU reference (of the fp16-rounded problem for the fp16 paths). Refused
        // before IsComplete and when a read-back holds fewer bytes than the shape needs.
        [[nodiscard]] Common::ResultStr<FusedMlpPathCheck> Complete() const;

        [[nodiscard]] NeuralKernelPath GetPath() const
        {
            return m_Path;
        }

    private:
        FusedMlpCheckRun() = default;

        NeuralKernelPath             m_Path = NeuralKernelPath::Fp32;
        std::string                  m_Tile;
        FusedMlpProblem              m_Problem;
        uint64_t                     m_SubmittedFrame = 0;
        Graphic::RDG::ExternalBuffer m_Output;
        Graphic::RDG::ExternalBuffer m_WeightGrad;
        Graphic::RDG::ExternalBuffer m_Activations;
    };
} // namespace Desert::Engine::Neural

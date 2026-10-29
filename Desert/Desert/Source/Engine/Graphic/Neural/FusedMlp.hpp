#pragma once

#include <Engine/Graphic/API/NeuralComputeCaps.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// THE FULLY FUSED MLP OF THE NN LAYER (NN-RUNTIME step 2): its shape, its memory layout, and its CPU fp32
// reference.
//
// The network is the one Müller et al. 2021 (Neural Radiance Caching) run: width 64, a configurable number of
// hidden layers, ReLU after each hidden layer, a linear output, no biases. Every layer is stored as one 64x64
// matrix — the first layer's unused input columns and the last layer's unused output rows are zero — so every
// kernel (FusedMlpCompute, Shaders/Neural/FusedMlp*.comp) runs one uniform 64x64 step per layer.
//
// Layouts shared by the CPU reference and the GPU kernels (all row-major, all padded to the width):
//   weights       [MatrixCount][64 out][64 in]    W_l[j][k]
//   input         [batch][64]                      h_0, columns >= InputDims are zero
//   output        [batch][64]                      y = W_last h_last (linear); columns >= OutputDims are zero
//   outputGrad    [batch][64]                      dL/dy supplied by the caller; columns >= OutputDims are zero
//   weightGrad    [MatrixCount][64][64]            dL/dW_l = sum over the batch of g_l (x) h_l
// where h_l is the input of matrix l (h_0 the network input, h_l = ReLU(W_{l-1} h_{l-1}) for l >= 1) and g_l is
// dL/d(W_l h_l): g_last = dL/dy, g_{l-1} = (W_l^T g_l) * [h_l > 0].
namespace Desert::Engine::Neural
{
    inline constexpr uint32_t kFusedMlpWidth = 64;

    // Batch rows one workgroup owns in every kernel; a batch is a multiple of it.
    inline constexpr uint32_t kFusedMlpBatchGranularity = 64;

    struct FusedMlpShape
    {
        uint32_t InputDims    = 0; ///< 1..64
        uint32_t OutputDims   = 0; ///< 1..64
        uint32_t HiddenLayers = 0; ///< >= 1; the network has HiddenLayers + 1 matrices

        [[nodiscard]] uint32_t MatrixCount() const
        {
            return HiddenLayers + 1;
        }
    };

    // Empty when the shape and batch are ones the kernels run; otherwise what is wrong.
    [[nodiscard]] std::string ValidateFusedMlp( const FusedMlpShape& shape, uint32_t batch );

    // One forward + backward problem: the weights, a batch of inputs and the loss gradient at the output.
    struct FusedMlpProblem
    {
        FusedMlpShape      Shape;
        uint32_t           Batch = 0;
        std::vector<float> Weights;    ///< MatrixCount * 64 * 64
        std::vector<float> Input;      ///< Batch * 64
        std::vector<float> OutputGrad; ///< Batch * 64
    };

    // What a forward + backward pass produces.
    struct FusedMlpResult
    {
        std::vector<float> Output;     ///< Batch * 64
        std::vector<float> WeightGrad; ///< MatrixCount * 64 * 64
        /// MatrixCount * 64 * 64: sum over the batch of m * |h|, m the gradient's propagated magnitude
        /// (m_last = |dL/dy|, m_{l-1} = [h_l > 0] |W_l|^T m_l), what every rounding of g and of the sum is a part
        /// of
        std::vector<float> WeightGradMagnitude;
        std::vector<float> Activations; ///< MatrixCount * Batch * 64: h_l, the input of matrix l (h_0 = input)
    };

    // THE REFERENCE OF AN FP16 PATH: the problem whose weights, inputs and output gradients are rounded to
    // the nearest fp16 (ties to even; subnormals kept). An fp16 network holds fp16 weights, so what it is
    // measured against is the exact MLP of THOSE weights, not of the fp32 ones it was handed; the remaining
    // difference is the kernel's own (fp16 activations and products).
    [[nodiscard]] FusedMlpProblem QuantizedToFp16( const FusedMlpProblem& problem );

    // How many hidden units (layers 1..MatrixCount-1, every batch row, every column) the two activation tensors
    // disagree on being active (h > 0): a flipped ReLU mask changes the gradient through that unit by O(1),
    // which is what pushes an fp16 weight-gradient error toward its tolerance.
    [[nodiscard]] uint64_t CountReluMaskFlips( const std::vector<float>& gpuActivations,
                                               const std::vector<float>& referenceActivations,
                                               const FusedMlpShape& shape, uint32_t batch );

    // A seeded problem: He-uniform weights (|w| <= sqrt(6 / fan_in)), inputs and output gradients uniform in
    // [-1, 1], zero outside the shape's dimensions. The same seed gives the same bytes on every machine.
    [[nodiscard]] FusedMlpProblem MakeSeededFusedMlpProblem( const FusedMlpShape& shape, uint32_t batch,
                                                             uint64_t seed );

    // The CPU fp32 reference: forward, then backward to the weight gradients, in the order the layouts above
    // define (sums accumulate in double so the reference is not the thing being measured).
    [[nodiscard]] FusedMlpResult RunFusedMlpReference( const FusedMlpProblem& problem );

    // The same reference with the backward run on `kernelActivations` (MatrixCount * Batch * 64, the layout of
    // FusedMlpResult::Activations), the KERNEL's stored h_l: both the ReLU masks [h_l > 0] and the h_l of every
    // dW_l = sum g_l (x) h_l. The backward and weight-gradient kernels are measured on the inputs they were
    // given. The fp16 forward's h_l carries an absolute error of a few fp16 ulps of sum |W| |h| (the forward
    // pre-activation's terms), which a near-zero unit turns into an O(1) relative one or a flipped mask; a
    // column k active in a handful of batch rows then has a weight gradient of a few terms, all of them
    // carrying that forward error, which the reference's own h_l would charge to the backward (the 3.4e-2 of
    // run 10-29 with masks alone). The forward is checked by Output and the flips are counted apart
    // (CountReluMaskFlips); Output and Activations stay the reference's own forward. nullopt when the tensor is
    // not that size.
    [[nodiscard]] std::optional<FusedMlpResult>
    RunFusedMlpReferenceOnActivations( const FusedMlpProblem&    problem,
                                       const std::vector<float>& kernelActivations );

    // max |gpu - ref| and that divided by max |ref| (the error relative to the tensor's scale: an element-wise
    // ratio is meaningless where a ReLU zeroes the reference).
    struct FusedMlpError
    {
        double MaxAbs = 0.0;
        double MaxRef = 0.0;
        double Rel    = 0.0;

        // CompareFusedMlpSum: the element that set Rel, matrix l, dW_l[Row][Column] (the next run names it).
        struct Element
        {
            bool     Valid     = false;
            uint32_t Matrix    = 0;
            uint32_t Row       = 0;
            uint32_t Column    = 0;
            double   Gpu       = 0.0;
            double   Reference = 0.0;
            double   Magnitude = 0.0;
        } Worst;
    };

    [[nodiscard]] FusedMlpError CompareFusedMlp( const std::vector<float>& gpu,
                                                 const std::vector<float>& reference );
    // A SUM IS MEASURED AGAINST ITS TERMS, NOT ITS RESULT: a weight gradient is a sum over the batch whose
    // signed terms cancel, so its rounding is bounded by the sum of the terms' magnitudes (Higham: |error| <=
    // gamma_n * sum |g| |h|), not by |sum|. Rel is the largest |gpu - reference| / magnitude over the elements;
    // the reference's WeightGradMagnitude also carries the rounding the backward put into g (see there).
    [[nodiscard]] FusedMlpError CompareFusedMlpSum( const std::vector<float>& gpu,
                                                    const std::vector<float>& reference,
                                                    const std::vector<float>& magnitude );

    // The relative tolerance a path's outputs are held to: 1e-2 for the fp16 paths, 1e-5 for fp32.
    [[nodiscard]] double FusedMlpTolerance( NeuralKernelPath path );
    // THE WEIGHT GRADIENT'S BOUND, derived, relative to WeightGradMagnitude: every backward layer rounds the
    // products, a 64-long sum and the store of g; the weight gradient rounds the product g h and a batch-long sum.
    // One ulp per rounding of the format it rounds to (products and stores: fp16 on the fp16 paths; sums: the
    // accumulator, always fp32: IsFp16Shape takes no fp16-accumulating tile). The 5-hidden-layer,
    // batch-1024 check: fp16 1.09e-2, fp32 1.6e-4.
    [[nodiscard]] double FusedMlpGradientTolerance( NeuralKernelPath path, const FusedMlpShape& shape,
                                                    uint32_t batch );
} // namespace Desert::Engine::Neural

#pragma once

#include <Engine/Graphic/API/NeuralComputeCaps.hpp>

#include <cstdint>
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
    };

    // A seeded problem: He-uniform weights (|w| <= sqrt(6 / fan_in)), inputs and output gradients uniform in
    // [-1, 1], zero outside the shape's dimensions. The same seed gives the same bytes on every machine.
    [[nodiscard]] FusedMlpProblem MakeSeededFusedMlpProblem( const FusedMlpShape& shape, uint32_t batch,
                                                             uint64_t seed );

    // The CPU fp32 reference: forward, then backward to the weight gradients, in the order the layouts above
    // define (sums accumulate in double so the reference is not the thing being measured).
    [[nodiscard]] FusedMlpResult RunFusedMlpReference( const FusedMlpProblem& problem );

    // max |gpu - ref| and that divided by max |ref| (the error relative to the tensor's scale: an element-wise
    // ratio is meaningless where a ReLU zeroes the reference).
    struct FusedMlpError
    {
        double MaxAbs = 0.0;
        double MaxRef = 0.0;
        double Rel    = 0.0;
    };

    [[nodiscard]] FusedMlpError CompareFusedMlp( const std::vector<float>& gpu,
                                                 const std::vector<float>& reference );

    // The relative tolerance a path is held to: 1e-2 for the fp16 paths, 1e-5 for fp32.
    [[nodiscard]] double FusedMlpTolerance( NeuralKernelPath path );
} // namespace Desert::Engine::Neural

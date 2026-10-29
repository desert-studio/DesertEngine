#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/NeuralComputeCaps.hpp>
#include <Engine/Graphic/Neural/FusedMlpCompute.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class ComputePipeline;
    class Shader;
} // namespace Desert::Graphic

namespace Desert::Graphic::RDG
{
    class Builder;
} // namespace Desert::Graphic::RDG

// THE NEURAL RADIANCE CACHE (NRC; Müller, Rousselle, Novák, Keller 2021, "Real-time Neural Radiance Caching for
// Path Tracing"), the first consumer of the NN layer (FusedMlpCompute). Contract: Notion Wiki «NRC — контракт».
// Setting PathTracing.RadianceCache = Off | Nrc (Common::Scalability::PathTracingRadianceCacheMethod).
//
// WHAT IT IS. A small network learns, online, the radiance L_r(x, w_o) a surface point reflects toward w_o (direct
// and indirect light, WITHOUT the point's own emission): input = a multiresolution hash-grid encoding of x (Müller
// et al. 2022, instant-ngp) plus one-blob encodings of w_o, the normal and the roughness plus the diffuse and
// specular reflectance; output = that radiance divided by (D_d + D_s) (the paper's factorisation: the network
// learns the light, not the texture). The fused MLP is FusedMlp.hpp's: width 64, kNrcHiddenLayers ReLU layers,
// linear output, no biases.
//
// WHERE THE PATH TRACER QUERIES IT (PathTracer.shader "PathTracing" built with PATH_TRACING_NRC=1). Each path
// keeps the paper's spread a = sum_{i>=2} sqrt( |x_{i-1} - x_i|^2 / ( p(w_i | x_{i-1}) |cos theta_i| ) ) from its
// second surface on, against a0 = |x_0 - x_1|^2 / ( 4 pi cos theta_1 ) of the camera segment. At an opaque
// surface x_n, n >= 2, after its emission was added and BEFORE its light samples, the path terminates into the
// cache when a^2 > kNrcSpreadThreshold * a0 or the bounce limit (PathTracing.MaxBounces) is reached: it writes a
// query (NrcQuery) and adds nothing more; the composite later adds T_n * cache(x_n) * (D_d + D_s). Glass
// interfaces (delta lobes) neither grow the spread nor terminate. The first surface is never cached.
//
// TRAINING PATHS. One pixel per kNrcTrainingTile-sided square tile (TileSide from the view size so a frame has
// about kNrcTrainingPaths of them; the pixel inside the tile is one per-frame random offset) traces its first
// path of the frame as a training path: it terminates into the cache for the IMAGE exactly as above, then keeps
// going (the unbiased suffix) and records up to kNrcMaxTrainingVertices opaque vertices (its first surface
// included): features, throughput T_i and the radiance R_i summed when vertex i's emission was added. The suffix
// ends by the same spread rule measured again from the image's termination vertex (or the bounce limit, a miss,
// Russian roulette); a suffix ended by the spread rule or the bounce limit queries the cache at its end
// (SELF-TRAINING, an NrcSelfQuery), except one training path in kNrcUnbiasedTrainingPaths, which never stops on
// the spread rule. Each vertex then gives one training sample (NrcTrainingSample): target
// L_i = (R_end - R_i) / T_i + (T_end / T_i) * cache(x_end) (the second term only with a self query; a vertex whose
// T_i has a channel below kNrcMinThroughput gives none).
//
// FRAME ORDER IN THE GRAPH (each a compute pass of this frame's graph, AddCachePasses; NeuralRadianceCache.shader
// passes in quotes):
//   "PathTracing: Trace"          the Nrc trace writes Queries, TrainingSamples, SelfQueries, Counters
//   per path s of the frame, per inference chunk c of its segment (kNrcInferenceChunk queries each)
//     "NRC: Encode"   Queries[c]   -> NnInput rows, from the INFERENCE table (HashEma, debiased)
//     "NN: Inference"              NnInput -> NnOutput with the INFERENCE weights (FusedMlpCompute::
//                                  AddInferencePass, NN_KERNEL 3)
//     "NRC: Composite"             AccumulationTexture.rgb += clamp(T * c), the denoiser's lobe sums likewise
//   "NRC: Encode" + "NN: Inference" on SelfQueries -> SelfOutput (the cache before this frame's training)
//   per training step s < kNrcTrainingSteps (kNrcTrainingBatch rows each, rows past the count masked)
//     "NRC: Encode"                TrainingSamples[s] -> NnInput
//     "NN: Forward"                (FusedMlpCompute::AddForwardPass) -> Output, Activations
//     "NRC: Loss"                  relative L2 vs the target -> NnOutputGrad, the step's loss partials
//     "NN: Backward", "NN: Weight gradient" (FusedMlpCompute::AddBackwardPasses) -> Grads, WeightGrad
//     "NRC: Input gradient"        dL/dh_0 = W_0^T g_0 of the 32 hash features, scattered into HashGrad
//     "NRC: Adam MLP"              Weights, AdamMlp moments; WeightsEma, InferenceWeights
//     "NRC: Adam Hash"             HashTable, AdamHash moments, HashEma; HashGrad back to zero
//   then the denoiser and the resolve as without the cache.
// Under Nrc the accumulation holds THIS frame's paths only (the cache they ended in changes every frame it
// trains, so earlier paths' cached radiance is stale): PathTracing.SamplesPerPixel caps the paths of a frame.
// The image's temporal integration is the denoiser's history instead (PathTracer: the per-frame restart of the
// sum under Nrc keeps that history, as a moving camera's does).
//
// THE INFERENCE WEIGHTS (Muller et al. 2021 §5): the image reads an exponential moving average of the trained
// parameters - MLP weights and hash table, decay kNrcEmaDecay per Adam step, debiased by 1 / (1 - decay^t) -
// not the optimiser's latest, so the cache's output is as stable over frames as the paper's; training, the loss
// and the self-training queries use the optimiser's own (the target the network is being moved toward). Before
// the first step (t = 0) the image reads the optimiser's (initial) parameters.
//
// THE PATH INTENSITY CLAMP: the cached contribution T * cache of a path is one contribution like each the path
// traced, so "NRC: Composite" applies PathTracer.shader's PathTracingAccumulate rule (largest channel at most
// MaxPathIntensity in radiance, hue kept) with the trace's own value (NrcCompositeTargets::PathIntensityClamp).
//
// THE QUERIES (live-sized): a path that ends in the cache claims a slot of its path's SEGMENT (path s of the frame
// -> Queries[s * capacity, (s + 1) * capacity), counter NrcCounter::Segments + s); a claim past the capacity is
// refused and that path goes on as without the cache (unbiased, only slower), so a too-small capacity costs time,
// never light. The capacity follows the read-back demand (NrcCounter::RenderQueries / paths, the peak of the last
// kNrcQueryDemandWindow read-backs, times kNrcQueryHeadroom, rounded up to kNrcInferenceChunk), starting at
// kNrcQueryStartShare of the pixels before the first read-back. A segment holds one path per pixel, so a chunk
// inside it composites each pixel at most once (no atomics).
//
// BUFFERS (persistent ones are the cache's; transients are this frame's graph):
//   persistent  Weights          fp32 [kNrcMatrixCount][64][64]           FusedMlp.hpp layout (training)
//               AdamMlp          fp32 [2][kNrcMatrixCount][64][64]        first, second moment
//               WeightsEma       fp32 like Weights: the moving average, from 0
//               InferenceWeights fp32 like Weights: WeightsEma debiased (what the image's inference reads)
//               HashTable        fp32 [kNrcHashLevels][kNrcHashTableSize][kNrcHashFeatures] (training)
//               AdamHash         fp32 [2][kNrcHashLevels][kNrcHashTableSize][kNrcHashFeatures]
//               HashEma          fp32 like HashTable: the moving average, from 0 (debiased in "NRC: Encode")
//               HashGrad         int32 like HashTable: fixed point, kNrcHashGradScale per unit (atomicAdd)
//   transient   Queries         NrcQuery [pathsPerFrame][capacity] (THE QUERIES)
//               TrainingSamples NrcTrainingSample [kNrcTrainingSamples]
//               SelfQueries     NrcVertex [kNrcMaxTrainingPaths]
//               Counters        uint [NrcCounter::Segments + pathsPerFrame], zero at the frame's start (an upload)
//               LossPartials    float [kNrcTrainingSteps][kNrcTrainingBatch / 64][2] (sum, valid rows)
//
// SHADER-SIDE TYPES: Editor/Resources/Shaders/Common/NeuralRadianceCache.glslh holds NrcVertex, NrcQuery,
// NrcTrainingSample, the packing (NrcPackVertex / NrcUnpackVertex), the input encoding (NrcEncode) and the
// constants below with the same names and values (static_asserts here on the sizes).
namespace Desert::Engine::Neural
{
    namespace RDG = Graphic::RDG;

    // ---- The network ----
    inline constexpr uint32_t      kNrcInputDims    = 58; // 32 hash + 8 w_o + 8 normal + 4 roughness + 3 + 3
    inline constexpr uint32_t      kNrcOutputDims   = 3;  // factorised RGB radiance
    inline constexpr uint32_t      kNrcHiddenLayers = 5;
    inline constexpr FusedMlpShape kNrcShape{
         .InputDims = kNrcInputDims, .OutputDims = kNrcOutputDims, .HiddenLayers = kNrcHiddenLayers };
    inline constexpr uint32_t kNrcMatrixCount = kNrcHiddenLayers + 1;

    // Input row columns (NrcEncode): [0, 32) hash features (level l at 2l, 2l + 1); [32, 40) w_o one-blob
    // (theta/pi 4 bins, phi/2pi 4 bins); [40, 48) normal the same; [48, 52) one-blob of 1 - exp(-roughness);
    // [52, 55) diffuse reflectance; [55, 58) specular reflectance; [58, 64) zero. One-blob bin i of k for v in
    // [0, 1]: exp( -((i + 0.5) / k - v)^2 / (2 sigma^2) ), sigma = 1 / k.
    inline constexpr uint32_t kNrcOneBlobBins = 4;

    // ---- The hash grid (instant-ngp): world position in cm, every level hashed (no dense levels) ----
    inline constexpr uint32_t kNrcHashLevels     = 16;
    inline constexpr uint32_t kNrcHashFeatures   = 2;
    inline constexpr uint32_t kNrcHashTableLog2  = 18;
    inline constexpr uint32_t kNrcHashTableSize  = 1u << kNrcHashTableLog2; // entries per level
    inline constexpr float    kNrcCoarsestCellCm = 1600.0f;                 // level 0 cell side
    inline constexpr float    kNrcFinestCellCm   = 4.0f;                    // level 15 cell side
    inline constexpr float    kNrcHashInitScale  = 1.0e-4f;                 // uniform +-, instant-ngp
    inline constexpr float    kNrcHashGradScale  = 16384.0f;                // fixed point per unit gradient
    inline constexpr float    kNrcHashGradClamp  = 16.0f;                   // per contribution, before scaling
    // Level l cell side = kNrcCoarsestCellCm * b^-l, b = (coarsest / finest)^(1 / (levels - 1)). Corner hash:
    // (x * 1u) ^ (y * 2654435761u) ^ (z * 805459861u) & (kNrcHashTableSize - 1), trilinear weights.

    // ---- Paths ----
    inline constexpr float    kNrcSpreadThreshold       = 0.01f; // c of a^2 > c * a0
    inline constexpr uint32_t kNrcMaxTrainingPaths      = 16384; // training paths a frame aims at
    inline constexpr uint32_t kNrcMaxTrainingVertices   = 8;     // samples one training path gives at most
    inline constexpr uint32_t kNrcUnbiasedTrainingPaths = 16;    // 1 in 16 never stops on the spread rule
    inline constexpr float    kNrcMinThroughput         = 1.0e-4f;
    inline constexpr float    kNrcAlbedoEpsilon         = 1.0e-3f; // the factorisation's divisor floor

    // ---- Training ----
    inline constexpr uint32_t kNrcTrainingBatch   = 16384;
    inline constexpr uint32_t kNrcTrainingSteps   = 4;
    inline constexpr uint32_t kNrcTrainingSamples = kNrcTrainingBatch * kNrcTrainingSteps; // capacity
    inline constexpr float    kNrcLossEpsilon     = 0.01f; // relative L2: (y - t)^2 / (sg(y)^2 + eps)
    inline constexpr float    kNrcLearningRate    = 5.0e-3f;
    inline constexpr float    kNrcAdamBeta1       = 0.9f;
    inline constexpr float    kNrcAdamBeta2       = 0.99f;
    inline constexpr float    kNrcAdamEpsilon     = 1.0e-8f;
    inline constexpr float    kNrcWeightDecay     = 1.0e-6f; // L2 on the MLP weights only (instant-ngp)
    inline constexpr float    kNrcEmaDecay        = 0.99f;   // THE INFERENCE WEIGHTS' average, per Adam step

    // ---- Inference ----
    // Queries per "NRC: Encode" + inference + composite: one chunk's input and output rows are 2 x 64 floats each
    // (8 MB + 8 MB), aliased chunk to chunk by the graph.
    inline constexpr uint32_t kNrcInferenceChunk = 1u << 15;
    // THE QUERIES' capacity per segment.
    inline constexpr float    kNrcQueryHeadroom     = 1.25f;
    inline constexpr uint32_t kNrcQueryDemandWindow = 8;     // read-backs whose peak demand sizes the capacity
    inline constexpr float    kNrcQueryStartShare   = 0.25f; // of the pixels, before the first read-back

    // Counters (uint [4]) the trace appends with.
    enum class NrcCounter : uint32_t
    {
        TrainingSamples =
             0, ///< NrcTrainingSample rows appended (may exceed kNrcTrainingSamples: the excess dropped)
        SelfQueries   = 1, ///< NrcVertex rows appended to SelfQueries
        TrainingPaths = 2, ///< training paths traced
        RenderQueries = 3, ///< paths of the image that asked to end in the cache (claims, refused ones included)
        Segments      = 4, ///< + s: the slots path s of the frame claimed in its segment (may exceed the capacity)
    };

    // GLSL mirrors (Common/NeuralRadianceCache.glslh), std430.
    // NrcVertex: A.xyz = floatBitsToUint(world position cm), A.w = packUnorm2x16(octahedral(w_o) * 0.5 + 0.5);
    // B.x = packUnorm2x16(octahedral(normal) * 0.5 + 0.5), B.y = packUnorm2x16(vec2(roughness, 0)),
    // B.z = packUnorm4x8(vec4(D_d, 0)), B.w = packUnorm4x8(vec4(D_s, 0)).
    struct NrcVertexGpu
    {
        glm::uvec4 A;
        glm::uvec4 B;
    };
    // NrcQuery (64 B, halves packed by packHalf2x16, saturated at 65504): the vertex; Lobes.xy = DiffuseWeight
    // (rgb T * (1 - share) / D_d1, a lum(I_d) of the path before the cache), Lobes.zw = SpecularWeight (rgb
    // T * share / D_s1, a lum(I_s) before the cache) (share = the first surface's bounce specular share, D_x1 its
    // guides: Common/PathTracingDenoise.glslh; both 0 when the path had no first-surface bounce); Weight.xy = T
    // (0: a path dropped after its claim adds nothing), Weight.z = the pixel y * width + x, Weight.w = 0.
    struct NrcQueryGpu
    {
        NrcVertexGpu Vertex;
        glm::uvec4   Lobes;
        glm::uvec4   Weight;
    };
    // NrcTrainingSample: the vertex, Radiance.rgb = (R_end - R_i) / T_i, Radiance.a = uintBitsToFloat(the self
    // query index, 0xFFFFFFFF none), Throughput.rgb = T_end / T_i.
    struct NrcTrainingSampleGpu
    {
        NrcVertexGpu Vertex;
        glm::vec4    Radiance;
        glm::vec4    Throughput;
    };
    static_assert( sizeof( NrcVertexGpu ) == 32 && sizeof( NrcQueryGpu ) == 64 &&
                   sizeof( NrcTrainingSampleGpu ) == 64 );

    // The trace's NRC push (PathTracer TracePush::Nrc, PathTracer.shader NrcParams / NrcSpread).
    struct NrcTraceParams
    {
        glm::uvec4 Params; ///< x = each segment's query capacity (> 0: Nrc), y = training tile side, z / w = this
                           ///< frame's pixel offset in the tile
        glm::vec4 Spread; ///< x = kNrcSpreadThreshold, y = kNrcUnbiasedTrainingPaths, z = kNrcMinThroughput, w = 0
    };

    // This frame's buffers the trace writes (AddTraceTargets) and the cache passes read.
    struct NrcTraceTargets
    {
        RDG::BufferRef Queries;
        RDG::BufferRef TrainingSamples;
        RDG::BufferRef SelfQueries;
        RDG::BufferRef Counters;
        NrcTraceParams Push;
        uint32_t       Width         = 0;
        uint32_t       Height        = 0;
        uint32_t       PathsPerFrame = 0;
        uint32_t       QueryCapacity = 0; ///< queries per segment (THE QUERIES)
    };

    // The images the composite adds the cached radiance to (PathTracer AccumulationRefs; this frame's paths only).
    struct NrcCompositeTargets
    {
        RDG::TextureRef Radiance;            ///< rgb += T * cache
        RDG::TextureRef DemodulatedDiffuse;  ///< rgb += DiffuseWeight * cache, a: the squared luminance's change
        RDG::TextureRef DemodulatedSpecular; ///< likewise with SpecularWeight
        float           PathIntensityClamp = 0.0f; ///< the trace's clamp in radiance (TracePush Sun.w; 0: none)
    };

    // What DesertCtl's state section "nrc" reports (ControlState): read back from frames whose fence passed.
    struct NrcStats
    {
        bool               Active = false;      ///< a frame ran the cache passes since the last reset
        std::string        KernelPath;          ///< NeuralKernelPathName of the path the MLP runs
        uint64_t           TrainingSteps   = 0; ///< Adam steps taken since the last reset
        uint32_t           TrainingSamples = 0; ///< samples of the last read-back frame (capped at capacity)
        uint32_t           TrainingPaths   = 0;
        uint32_t           SelfQueries     = 0;
        uint32_t           RenderQueries    = 0; ///< paths that asked to end in the cache (the demand)
        uint32_t           QueryCapacity    = 0; ///< queries per segment of the last frame built (THE QUERIES)
        uint64_t           QueryMemoryBytes = 0; ///< its Queries buffer + one inference chunk's input and output
        float              TrainLoss       = 0; ///< mean relative L2 of the last read-back frame's steps
        float              FirstTrainLoss  = 0; ///< the first read-back after the last reset
        std::vector<float> LossHistory;         ///< the last kNrcLossHistory read-backs, oldest first
        std::string        Error;               ///< the last refusal, empty when none
    };
    inline constexpr size_t kNrcLossHistory = 32;

    class NeuralRadianceCache
    {
    public:
        explicit NeuralRadianceCache( const NeuralComputeCaps& caps );
        ~NeuralRadianceCache();

        /// The registered program of the cache's own passes (NeuralRadianceCache.shader: "<kProgram>/<Pass>").
        static constexpr const char* kProgram = "NeuralRadianceCache";
        /// Every registered program the cache's passes and its MLP's kernels are built from: a recompile of any
        /// of them (ShaderService::ProgramGeneration) changes the code that trained its weights.
        static constexpr std::array<const char*, 2> kPrograms = { kProgram, FusedMlpCompute::kProgram };

        NeuralRadianceCache( const NeuralRadianceCache& )            = delete;
        NeuralRadianceCache& operator=( const NeuralRadianceCache& ) = delete;

        // The NeuralRadianceCache.shader programs and pipelines, and the MLP path (ResolveNeuralKernelPath with
        // MachineSettings::ForcedNeuralKernelPath), with every MLP kernel of that path built. Refused naming what
        // is missing; a refusal of the shader service is held (GetShaderRefusal) for its reporter.
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The program variant the shader service refused in the last Initialize, empty when that refusal was not
        // the service's (the kernel path, a pipeline, a binding layout) or Initialize succeeded.
        [[nodiscard]] const std::optional<NeuralShaderRefusal>& GetShaderRefusal() const
        {
            return m_ShaderRefusal;
        }

        // A new world: the next frame starts from freshly initialised weights and hash table, the stats empty.
        void Reset();

        // GRAPH BUILD, before the trace: this frame's transients the Nrc trace writes (Counters uploaded zero)
        // and its push. @p frameSeed picks the training pixel offset.
        [[nodiscard]] Common::ResultStr<NrcTraceTargets> AddTraceTargets( RDG::Builder& graph, uint32_t width,
                                                                          uint32_t height, uint32_t pathsPerFrame,
                                                                          uint32_t frameSeed );

        // GRAPH BUILD, after the trace: inference + composite, the self-training inference, kNrcTrainingSteps
        // training steps, the stats read-back (FRAME ORDER above). Refuses adding nothing when a pipeline or the
        // NN layer refuses.
        [[nodiscard]] Common::BoolResultStr AddCachePasses( RDG::Builder& graph, const NrcTraceTargets& trace,
                                                            const NrcCompositeTargets& composite );

        [[nodiscard]] const NrcStats& GetStats() const
        {
            return m_Stats;
        }

    private:
        struct Program
        {
            const char*                               Pass = "";
            std::shared_ptr<Graphic::Shader>          Shader;
            std::shared_ptr<Graphic::ComputePipeline> Pipeline;
            mutable Graphic::ShaderBindingLayoutCache Layout;
        };
        struct Persistent
        {
            RDG::ExternalBuffer Weights;
            RDG::ExternalBuffer AdamMlp;
            RDG::ExternalBuffer HashTable;
            RDG::ExternalBuffer AdamHash;
            RDG::ExternalBuffer HashGrad;
            RDG::ExternalBuffer WeightsEma;
            RDG::ExternalBuffer InferenceWeights;
            RDG::ExternalBuffer HashEma;
            bool                Built = false;
        };
        struct PersistentRefs
        {
            RDG::BufferRef Weights;
            RDG::BufferRef AdamMlp;
            RDG::BufferRef HashTable;
            RDG::BufferRef AdamHash;
            RDG::BufferRef HashGrad;
            RDG::BufferRef WeightsEma;
            RDG::BufferRef InferenceWeights;
            RDG::BufferRef HashEma;
        };
        // One frame's read-back: the counters and the loss partials, extracted HostRead.
        struct ReadBack
        {
            RDG::ExternalBuffer Counters;
            RDG::ExternalBuffer LossPartials;
            uint64_t            Frame   = 0;
            uint32_t            Paths   = 1; ///< the frame's paths per pixel (its segments)
            bool                Pending = false;
        };

        [[nodiscard]] PersistentRefs AcquirePersistent( RDG::Builder& graph );
        // @p hashTable / @p hashScale: the table the hash features are read from and their scale (THE INFERENCE
        // WEIGHTS); @p capacity / @p countBase: the counted range's capacity and first record (a render segment;
        // 0: the training samples' or self queries' own).
        [[nodiscard]] RDG::BufferRef AddEncodePass( RDG::Builder& graph, RDG::BufferRef hashTable, float hashScale,
                                                    RDG::BufferRef source, uint32_t strideVec4, uint32_t firstRow,
                                                    uint32_t rows, RDG::BufferRef counters, uint32_t countIndex, uint32_t capacity = 0, uint32_t countBase = 0 );
        void                         CollectReadBacks();

        NeuralComputeCaps                m_Caps;
        NeuralKernelPath                 m_Path = NeuralKernelPath::Fp32;
        std::unique_ptr<FusedMlpCompute> m_Mlp;
        Program                          m_Encode{ .Pass = "Encode" };
        Program                          m_Loss{ .Pass = "Loss" };
        Program                          m_InputGrad{ .Pass = "InputGrad" };
        Program                          m_AdamMlp{ .Pass = "AdamMlp" };
        Program                          m_AdamHash{ .Pass = "AdamHash" };
        Program                          m_Composite{ .Pass = "Composite" };
        Persistent                       m_Persistent;
        std::vector<ReadBack>            m_ReadBacks; ///< MaxFramesInFlight + 1 slots
        uint64_t                         m_AdamStep    = 0;
        uint32_t                         m_QueryCapacity = 0;  ///< THE QUERIES' capacity per segment, 0: start
        std::vector<uint32_t>            m_QueryDemand;        ///< per-segment demand of the last read-backs
        bool                             m_Initialized = false;
        std::optional<NeuralShaderRefusal> m_ShaderRefusal; ///< GetShaderRefusal
        NrcStats                         m_Stats;
    };
} // namespace Desert::Engine::Neural

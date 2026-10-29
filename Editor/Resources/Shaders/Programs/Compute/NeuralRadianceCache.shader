// DesertAsset {"Kind":"Shader","Guid":"5a0536eda5704d4d9b1c14df060ed156","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEURAL RADIANCE CACHE'S KERNELS (Engine/Graphic/Neural/NeuralRadianceCache.hpp holds the contract: the frame
// order, the buffers, the constants; Common/NeuralRadianceCache.glslh the records and the encoding). The MLP itself
// is NeuralFusedMlp.shader's; these are the passes around it: the input encoding, the loss, the gradient into the
// hash grid, the two Adam updates and the composite of the cached radiance into the path tracer's sums.
//
// Every pass runs one invocation per row (a record, a batch row or a parameter) in groups of 64 and has the same
// push block: Rows / Extra / Adam / Adam2, whose fields each pass names below.
Shader "NeuralRadianceCache"
{
    // RECORDS -> MLP INPUT ROWS. Rows: x = record stride in uvec4 (the vertex is the record's first two), y = first
    // record, z = rows dispatched, w = records in the source. Extra: x = the counter bounding the valid records
    // (0xFFFFFFFF: none), y = that counter's capacity, z = the record its count starts at (a render query
    // segment's base; 0 otherwise). Adam.x = the scale of the hash features read (the debiasing of the hash
    // table's moving average for the render queries; 1 for the training table). Row r encodes record y + r, or
    // zeros past the valid ones.
    Pass "Encode"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>

            ReadBuffer(0) NrcSourceUB { uvec4 nrcSource[]; };
            ReadBuffer(1) NrcHashTableUB { float nrcHashTable[]; }; // [levels][table size][features]
            ReadBuffer(2) NrcCountersUB { uint nrcCounters[]; };
            Buffer(3) NnInput { float nnInput[]; };                // [rows][64]

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            void main()
            {
                const uint row = gl_GlobalInvocationID.x;
                if (row >= Rows.z)
                    return;
                const uint record = Rows.y + row;
                uint       valid  = Rows.w;
                if (Extra.x != 0xFFFFFFFFu)
                    valid = min(valid, Extra.z + min(nrcCounters[Extra.x], Extra.y));
                const uint out0 = row * 64u;
                if (record >= valid)
                {
                    for (uint c = 0u; c < 64u; ++c)
                        nnInput[out0 + c] = 0.0;
                    return;
                }

                NrcVertex packed;
                packed.A = nrcSource[record * Rows.x];
                packed.B = nrcSource[record * Rows.x + 1u];
                const NrcVertexData d = NrcUnpackVertex(packed);

                // [0, 32): the hash grid, trilinear per level.
                for (uint level = 0u; level < NRC_HASH_LEVELS; ++level)
                {
                    vec2 feature = vec2(0.0);
                    for (uint corner = 0u; corner < 8u; ++corner)
                    {
                        uint  entry;
                        float weight;
                        NrcHashCorner(d.Position, level, corner, entry, weight);
                        feature += weight * vec2(nrcHashTable[entry * NRC_HASH_FEATURES],
                                                 nrcHashTable[entry * NRC_HASH_FEATURES + 1u]);
                    }
                    nnInput[out0 + level * NRC_HASH_FEATURES]      = feature.x * Adam.x;
                    nnInput[out0 + level * NRC_HASH_FEATURES + 1u] = feature.y * Adam.x;
                }
                // [32, 64): w_o, normal, roughness, reflectances, zero padding.
                for (uint c = NRC_HASH_LEVELS * NRC_HASH_FEATURES; c < 64u; ++c)
                    nnInput[out0 + c] = NrcEncodeColumn(d, c);
            }
        }
    }

    // THE RELATIVE L2 LOSS (Muller et al. 2021: (y - t)^2 / (sg(y)^2 + eps), y treated as a constant in the
    // denominator) of one training step's rows against their targets, and its gradient at the output. Rows: x = the
    // step, y = the batch, z = the self queries' capacity. The target of sample i is its suffix radiance plus, with a
    // self query, its suffix throughput times the cache at the suffix end (this frame's cache before training),
    // both divided by the vertex's factor (the network learns radiance / (D_d + D_s)). The gradient is per row, not
    // divided by the batch: the Adam passes divide by the step's valid rows. Each group writes its rows' loss sum
    // and valid count (the stats read-back).
    Pass "Loss"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>

            ReadBuffer(0) NrcTrainingSamplesUB { NrcTrainingSample nrcSamples[]; };
            ReadBuffer(1) NrcSelfQueriesUB { NrcVertex nrcSelfQueries[]; };
            ReadBuffer(2) NrcSelfOutputUB { float nrcSelfOutput[]; }; // [self rows][64]
            ReadBuffer(3) NnOutput { float nnOutput[]; };             // [batch][64]
            ReadBuffer(4) NrcCountersUB { uint nrcCounters[]; };
            Buffer(5) NnOutputGrad { float nnOutputGrad[]; };          // [batch][64]
            Buffer(6) NrcLossPartialsUB { float nrcLossPartials[]; };  // [steps][batch / 64][2]

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            shared float sLoss[64];
            shared float sValid[64];

            void main()
            {
                const uint row    = gl_GlobalInvocationID.x;
                const uint index  = Rows.x * Rows.y + row;
                const uint out0   = row * 64u;
                const uint count  = min(nrcCounters[NRC_COUNTER_TRAINING_SAMPLES], NRC_TRAINING_SAMPLES);

                float loss  = 0.0;
                bool  valid = row < Rows.y && index < count;
                vec3  grad  = vec3(0.0);
                if (valid)
                {
                    const NrcTrainingSample s = nrcSamples[index];
                    vec3       target         = s.Radiance.rgb;
                    const uint self           = floatBitsToUint(s.Radiance.a);
                    if (self != NRC_NO_SELF_QUERY && self < min(nrcCounters[NRC_COUNTER_SELF_QUERIES], Rows.z))
                    {
                        const vec3 cached = max(vec3(nrcSelfOutput[self * 64u], nrcSelfOutput[self * 64u + 1u],
                                                     nrcSelfOutput[self * 64u + 2u]), vec3(0.0));
                        target += s.Throughput.rgb * cached * NrcFactor(NrcUnpackVertex(nrcSelfQueries[self]));
                    }
                    target /= NrcFactor(NrcUnpackVertex(s.Vertex));
                    const vec3 y     = vec3(nnOutput[out0], nnOutput[out0 + 1u], nnOutput[out0 + 2u]);
                    const vec3 scale = 1.0 / (y * y + NRC_LOSS_EPSILON);
                    const vec3 d     = y - target;
                    grad             = 2.0 * d * scale;
                    loss             = dot(d * d, scale) / 3.0;
                    // A non-finite target (a firefly past the float range) teaches nothing: the row is dropped.
                    if (isnan(loss) || isinf(loss) || any(isnan(grad)) || any(isinf(grad)))
                    {
                        valid = false;
                        loss  = 0.0;
                        grad  = vec3(0.0);
                    }
                }
                if (row < Rows.y)
                {
                    nnOutputGrad[out0]      = grad.x;
                    nnOutputGrad[out0 + 1u] = grad.y;
                    nnOutputGrad[out0 + 2u] = grad.z;
                    for (uint c = NRC_OUTPUT_DIMS; c < 64u; ++c)
                        nnOutputGrad[out0 + c] = 0.0;
                }

                sLoss[gl_LocalInvocationIndex]  = loss;
                sValid[gl_LocalInvocationIndex] = valid ? 1.0 : 0.0;
                barrier();
                for (uint stride = 32u; stride > 0u; stride >>= 1u)
                {
                    if (gl_LocalInvocationIndex < stride)
                    {
                        sLoss[gl_LocalInvocationIndex] += sLoss[gl_LocalInvocationIndex + stride];
                        sValid[gl_LocalInvocationIndex] += sValid[gl_LocalInvocationIndex + stride];
                    }
                    barrier();
                }
                if (gl_LocalInvocationIndex == 0u)
                {
                    const uint partial               = Rows.x * (Rows.y / 64u) + gl_WorkGroupID.x;
                    nrcLossPartials[partial * 2u]      = sLoss[0];
                    nrcLossPartials[partial * 2u + 1u] = sValid[0];
                }
            }
        }
    }

    // THE GRADIENT INTO THE HASH GRID: dL/dh_0 = W_0^T g_0 for the 32 hash columns of a valid row, spread over the
    // eight corners of each level by their trilinear weights and summed into HashGrad as fixed point
    // (NRC_HASH_GRAD_SCALE per unit, each contribution clamped to NRC_HASH_GRAD_CLAMP): integer atomics need no
    // device feature and sum in any order to the same bits. Rows: x = the step, y = the batch. NN_PATH as in
    // NeuralFusedMlp.shader: the fp16 paths store g_l as float16.
    Pass "InputGrad"
    {
        Compute
        {
            #ifndef NN_PATH
            #define NN_PATH 0
            #endif
            #if NN_PATH != 0
            #extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
            #extension GL_EXT_shader_16bit_storage : require
            #define nn_t float16_t
            #else
            #define nn_t float
            #endif

            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>

            ReadBuffer(0) NnWeights { float nnWeights[]; }; // [matrices][64 out][64 in]
            ReadBuffer(1) NnGrads { nn_t nnGrads[]; };       // [matrices][batch][64]; layer 0 first
            ReadBuffer(2) NrcTrainingSamplesUB { NrcTrainingSample nrcSamples[]; };
            ReadBuffer(3) NrcCountersUB { uint nrcCounters[]; };
            Buffer(4) NrcHashGradUB { int nrcHashGrad[]; };  // [levels][table size][features], fixed point

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            void main()
            {
                const uint row    = gl_GlobalInvocationID.x;
                const uint index  = Rows.x * Rows.y + row;
                if (row >= Rows.y || index >= min(nrcCounters[NRC_COUNTER_TRAINING_SAMPLES], NRC_TRAINING_SAMPLES))
                    return;
                const vec3 position = NrcUnpackVertex(nrcSamples[index].Vertex).Position;
                const uint g0       = row * 64u;
                for (uint level = 0u; level < NRC_HASH_LEVELS; ++level)
                {
                    vec2 dh = vec2(0.0);
                    for (uint j = 0u; j < 64u; ++j)
                    {
                        const float g = float(nnGrads[g0 + j]);
                        dh.x += nnWeights[j * 64u + level * NRC_HASH_FEATURES] * g;
                        dh.y += nnWeights[j * 64u + level * NRC_HASH_FEATURES + 1u] * g;
                    }
                    if (all(equal(dh, vec2(0.0))) || any(isnan(dh)) || any(isinf(dh)))
                        continue;
                    for (uint corner = 0u; corner < 8u; ++corner)
                    {
                        uint  entry;
                        float weight;
                        NrcHashCorner(position, level, corner, entry, weight);
                        const vec2 q = round(clamp(weight * dh, vec2(-NRC_HASH_GRAD_CLAMP), vec2(NRC_HASH_GRAD_CLAMP)) *
                                             NRC_HASH_GRAD_SCALE);
                        if (q.x != 0.0)
                            atomicAdd(nrcHashGrad[entry * NRC_HASH_FEATURES], int(q.x));
                        if (q.y != 0.0)
                            atomicAdd(nrcHashGrad[entry * NRC_HASH_FEATURES + 1u], int(q.y));
                    }
                }
            }
        }
    }

    // ADAM ON THE MLP WEIGHTS (Kingma & Ba 2015; the instant-ngp settings, L2 weight decay on the MLP only). Rows:
    // x = the step, y = the batch, z = the weights, w = Adam's t (>= 1). Adam: lr, beta1, beta2, eps; Adam2.x = the
    // weight decay. A step with no valid rows changes no weight.
    // THEN THE MOVING AVERAGE THE IMAGE IS INFERRED WITH (Muller et al. 2021 §5: an exponential moving average of
    // the weights against the cache's temporal flicker; the training weights stay the optimiser's own): every step,
    // valid rows or not, ema = Adam2.y * ema + (1 - Adam2.y) * w, and the inference weights = ema * Adam2.z (the
    // debiasing 1 / (1 - Adam2.y^t) of an average that starts at 0).
    Pass "AdamMlp"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>

            ReadBuffer(0) NnWeightGrad { float nnWeightGrad[]; }; // [matrices][64][64], summed over the batch
            Buffer(1) NrcWeightsUB { float nrcWeights[]; };
            Buffer(2) NrcAdamMlpUB { float nrcAdamMlp[]; };       // [2][weights]: first, second moment
            ReadBuffer(3) NrcCountersUB { uint nrcCounters[]; };
            Buffer(4) NrcWeightsEmaUB { float nrcWeightsEma[]; };             // the average, from 0
            Buffer(5) NrcInferenceWeightsUB { float nrcInferenceWeights[]; }; // the debiased average

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            void main()
            {
                const uint e = gl_GlobalInvocationID.x;
                if (e >= Rows.z)
                    return;
                const int count = int(min(nrcCounters[NRC_COUNTER_TRAINING_SAMPLES], NRC_TRAINING_SAMPLES));
                const int rows  = clamp(count - int(Rows.x * Rows.y), 0, int(Rows.y));
                float w = nrcWeights[e];
                if (rows > 0)
                {
                    const float g = nnWeightGrad[e] / float(rows) + Adam2.x * w;
                    if (!isnan(g) && !isinf(g))
                    {
                        const float m      = Adam.y * nrcAdamMlp[e] + (1.0 - Adam.y) * g;
                        const float v      = Adam.z * nrcAdamMlp[Rows.z + e] + (1.0 - Adam.z) * g * g;
                        nrcAdamMlp[e]          = m;
                        nrcAdamMlp[Rows.z + e] = v;
                        const float t      = float(Rows.w);
                        const float mHat   = m / (1.0 - pow(Adam.y, t));
                        const float vHat   = v / (1.0 - pow(Adam.z, t));
                        w                  = w - Adam.x * mHat / (sqrt(vHat) + Adam.w);
                        nrcWeights[e]      = w;
                    }
                }
                const float ema        = Adam2.y * nrcWeightsEma[e] + (1.0 - Adam2.y) * w;
                nrcWeightsEma[e]       = ema;
                nrcInferenceWeights[e] = ema * Adam2.z;
            }
        }
    }

    // ADAM ON THE HASH TABLE: the fixed-point gradient back to float, divided by the step's valid rows, and the
    // gradient zeroed for the next step. Rows / Adam as "AdamMlp" (no weight decay), and the table's moving average
    // likewise every step (Adam2.y the decay; its debiasing is applied where the render queries read it, "Encode"
    // Adam.x, as the encoding is linear in the table). Dispatched 2D: element e = x + y * (groups x * 64).
    Pass "AdamHash"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>

            Buffer(0) NrcHashTableUB { float nrcHashTable[]; };
            Buffer(1) NrcAdamHashUB { float nrcAdamHash[]; }; // [2][entries * features]
            Buffer(2) NrcHashGradUB { int nrcHashGrad[]; };
            ReadBuffer(3) NrcCountersUB { uint nrcCounters[]; };
            Buffer(4) NrcHashEmaUB { float nrcHashEma[]; }; // the average, from 0

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            void main()
            {
                const uint e = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * (gl_NumWorkGroups.x * 64u);
                if (e >= Rows.z)
                    return;
                const int fixedGrad = nrcHashGrad[e];
                nrcHashGrad[e]      = 0;
                const int count     = int(min(nrcCounters[NRC_COUNTER_TRAINING_SAMPLES], NRC_TRAINING_SAMPLES));
                const int rows      = clamp(count - int(Rows.x * Rows.y), 0, int(Rows.y));
                float value = nrcHashTable[e];
                if (rows > 0)
                {
                    const float g      = float(fixedGrad) / (NRC_HASH_GRAD_SCALE * float(rows));
                    const float m      = Adam.y * nrcAdamHash[e] + (1.0 - Adam.y) * g;
                    const float v      = Adam.z * nrcAdamHash[Rows.z + e] + (1.0 - Adam.z) * g * g;
                    nrcAdamHash[e]          = m;
                    nrcAdamHash[Rows.z + e] = v;
                    const float t      = float(Rows.w);
                    const float mHat   = m / (1.0 - pow(Adam.y, t));
                    const float vHat   = v / (1.0 - pow(Adam.z, t));
                    value              = value - Adam.x * mHat / (sqrt(vHat) + Adam.w);
                    nrcHashTable[e]    = value;
                }
                nrcHashEma[e] = Adam2.y * nrcHashEma[e] + (1.0 - Adam2.y) * value;
            }
        }
    }

    // THE CACHED RADIANCE INTO THE PATH TRACER'S SUMS: each query of the chunk that ended in the cache adds T times
    // the cache (the network's output times the vertex's factor) to its pixel's radiance, and its lobe weights
    // times the cache to the denoiser's demodulated sums, whose .a (the paths' squared luminance) changes by the
    // square of the path's new lobe luminance minus its old one. The cached contribution T * cache is a contribution
    // of the path like any it traced, so it is scaled down by PathTracer.shader's PathTracingAccumulate rule (hue
    // kept, its largest channel at most Adam.x = MaxPathIntensity in radiance; 0: no clamp), its lobe shares with
    // it. Rows: x = the segment's first query (path s * capacity), y = the chunk's rows, z = the chunk's first row
    // in the segment, w = the view's width. Extra: x = the segment's counter, y = its capacity. A segment holds one
    // path of each pixel at most, so a chunk (inside one segment) at most one query per pixel.
    Pass "Composite"
    {
        Compute
        {
            LocalSize(64, 1, 1);

            #include <Common/NeuralRadianceCache.glslh>
            #include <Common/PathTracingDenoise.glslh>

            ReadBuffer(0) NrcQueriesUB { NrcQuery nrcQueries[]; };
            ReadBuffer(1) NnOutput { float nnOutput[]; }; // [chunk][64]
            layout(binding = 2, rgba32f) uniform image2D AccumulationTexture;
            layout(binding = 3, rgba32f) uniform image2D DemodulatedDiffuseTexture;
            layout(binding = 4, rgba32f) uniform image2D DemodulatedSpecularTexture;
            ReadBuffer(5) NrcCountersUB { uint nrcCounters[]; };

            PushConstant NrcPush
            {
                uvec4 Rows;
                uvec4 Extra;
                vec4  Adam;
                vec4  Adam2;
            };

            vec4 NrcAddLobe(vec4 sum, vec4 weight, vec3 cached)
            {
                const vec3  added = weight.rgb * cached;
                const float lum   = weight.a + PathTracingDenoiseLuminance(added);
                return sum + vec4(added, lum * lum - weight.a * weight.a);
            }

            void main()
            {
                const uint row = gl_GlobalInvocationID.x;
                if (row >= Rows.y)
                    return;
                const uint index = Rows.z + row;
                if (index >= min(nrcCounters[Extra.x], Extra.y))
                    return;
                const NrcQuery     q = nrcQueries[Rows.x + index];
                const NrcQueryData d = NrcUnpackQuery(q);
                if (all(equal(d.Throughput, vec3(0.0))))
                    return;
                const ivec2 pixel  = ivec2(d.Pixel % Rows.w, d.Pixel / Rows.w);
                vec3        cached = max(vec3(nnOutput[row * 64u], nnOutput[row * 64u + 1u], nnOutput[row * 64u + 2u]),
                                         vec3(0.0)) *
                                     NrcFactor(NrcUnpackVertex(q.Vertex));
                const vec3  contribution = d.Throughput * cached;
                const float brightest    = max(contribution.r, max(contribution.g, contribution.b));
                if (Adam.x > 0.0 && brightest > Adam.x)
                    cached *= Adam.x / brightest;
                const vec4 radiance = imageLoad(AccumulationTexture, pixel);
                imageStore(AccumulationTexture, pixel, vec4(radiance.rgb + d.Throughput * cached, radiance.a));
                imageStore(DemodulatedDiffuseTexture, pixel,
                           NrcAddLobe(imageLoad(DemodulatedDiffuseTexture, pixel), d.DiffuseWeight, cached));
                imageStore(DemodulatedSpecularTexture, pixel,
                           NrcAddLobe(imageLoad(DemodulatedSpecularTexture, pixel), d.SpecularWeight, cached));
            }
        }
    }
}

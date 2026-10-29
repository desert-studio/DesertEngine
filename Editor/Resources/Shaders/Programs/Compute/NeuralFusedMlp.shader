// DesertAsset {"Kind":"Shader","Guid":"7c556162612c4426a5cc1ad440a7b86f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "NeuralFusedMlp"
{
    Compute
    {
        // THE NN LAYER'S FULLY FUSED MLP (NN-RUNTIME step 2; Engine/Graphic/Neural/FusedMlp.hpp holds the layouts
        // and the CPU fp32 reference this is checked against, FusedMlpCompute.cpp the dispatches). Muller et al.
        // 2021 / tiny-cuda-nn FullyFusedMLP: width 64, ReLU hidden layers, linear output, no biases; one workgroup
        // owns 64 batch rows and keeps their activations and the current layer's 64x64 weights in shared memory
        // for the whole network, so a layer never round-trips through global memory.
        //
        // Variants (FusedMlpCompute::KernelVariant), each a define:
        //   NN_KERNEL  0 forward      h_{l+1} = ReLU(W_l h_l), y = W_last h_last; writes every h_l (training)
        //              1 backward     g_{l-1} = (W_l^T g_l) * [h_l > 0] from g_last = dL/dy; writes every g_l
        //              2 weight grad  dW_l = sum over the batch of g_l (x) h_l, fp32, deterministic (no atomics)
        //              3 inference    the forward of 0 that keeps no activations: reads NnWeights NnInput, writes
        //                             NnOutput only (the neural radiance cache's queries, NeuralRadianceCache.hpp)
        //   NN_PATH    0 fp32 scalar, 1 fp16 scalar (fp16 products, fp32 sums), 2 cooperative matrix fp16
        //   NN_TM / NN_TN / NN_TK   the cooperative-matrix tile the device selected (NeuralComputeCaps::Tile)
        //   NN_ACC_FP16             the tile accumulates in fp16 (a device with no fp32-accumulating fp16 shape)
        // The default variant (no define) is fp32 forward, so the program the boot compiles is a real one.
        #ifndef NN_KERNEL
        #define NN_KERNEL 0
        #endif
        #ifndef NN_PATH
        #define NN_PATH 0
        #endif

        #if NN_PATH != 0
        #extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
        #extension GL_EXT_shader_16bit_storage : require
        #endif
        #if NN_PATH == 2
        #extension GL_KHR_cooperative_matrix : require
        #extension GL_KHR_memory_scope_semantics : require
        #extension GL_KHR_shader_subgroup_basic : require
        #endif

        #define NN_WIDTH 64
        #define NN_BLOCK 64
        #define NN_BLOCK_ELEMENTS (NN_BLOCK * NN_WIDTH)
        #define NN_MATRIX_ELEMENTS (NN_WIDTH * NN_WIDTH)

        #if NN_PATH == 0
        #define nn_t float
        #else
        #define nn_t float16_t
        #endif

        #if NN_PATH == 2
        #ifdef NN_ACC_FP16
        #define nn_acc_t float16_t
        #else
        #define nn_acc_t float
        #endif
        #define NN_THREADS 128
        #else
        #define NN_THREADS 256
        #endif

        LocalSize(NN_THREADS, 1, 1);

        PushConstant NeuralMlpPush
        {
            uint Batch;       // rows, a multiple of NN_BLOCK
            uint MatrixCount; // hidden layers + 1
        } Push;

        // fp32 in, as the caller holds them; the fp16 paths convert on load. Each kernel declares only the buffers
        // it touches, with the access it makes (reflection lists every declared buffer, and the render-graph pass
        // binds exactly these: FusedMlpCompute::AddTrainingPasses).
        #if NN_KERNEL == 0 || NN_KERNEL == 3
        ReadBuffer(0) NnWeights { float Weights[]; };       // [MatrixCount][64 out][64 in]
        ReadBuffer(1) NnInput { float Input[]; };           // [Batch][64]
        Buffer(3) NnOutput { float Output[]; };             // [Batch][64]
        #if NN_KERNEL == 0
        Buffer(4) NnActivations { nn_t Activations[]; };    // [MatrixCount][Batch][64] h_l, the input of matrix l
        #endif
        #elif NN_KERNEL == 1
        ReadBuffer(0) NnWeights { float Weights[]; };       // [MatrixCount][64 out][64 in]
        ReadBuffer(2) NnOutputGrad { float OutputGrad[]; }; // [Batch][64] dL/dy
        ReadBuffer(4) NnActivations { nn_t Activations[]; };
        Buffer(5) NnGrads { nn_t Grads[]; };                // [MatrixCount][Batch][64] g_l
        #else
        ReadBuffer(4) NnActivations { nn_t Activations[]; };
        ReadBuffer(5) NnGrads { nn_t Grads[]; };
        Buffer(6) NnWeightGrad { float WeightGrad[]; };     // [MatrixCount][64][64]
        #endif

        uint RowIndex( uint layer, uint row )
        {
            return ( layer * Push.Batch + row ) * NN_WIDTH;
        }

        #if NN_KERNEL != 2
        // The block's rows (activations forward, gradients backward) and the layer being applied.
        shared nn_t sRows[NN_BLOCK_ELEMENTS];
        shared nn_t sWeights[NN_MATRIX_ELEMENTS];

        void LoadWeights( uint layer )
        {
            for ( uint i = gl_LocalInvocationIndex; i < NN_MATRIX_ELEMENTS; i += NN_THREADS )
                sWeights[i] = nn_t( Weights[layer * NN_MATRIX_ELEMENTS + i] );
        }
        #endif

        #if NN_PATH == 2 && NN_KERNEL != 2
        // A tile's result before the element-wise step (ReLU / the ReLU mask): KHR cooperative matrices do not
        // say which lane holds which (row, column), so the step reads it back from here by position.
        shared nn_acc_t sTiles[NN_BLOCK_ELEMENTS];
        #endif

        // ---------------------------------------------------------------------------------------------------
        // Forward / backward: one 64x64 step per layer, the block's rows times the layer's matrix.
        //   forward   out[r][j] = sum_k W[j][k] * rows[r][k]   (rows x W^T)
        //   backward  out[r][k] = sum_j W[j][k] * rows[r][j]   (rows x W)
        // ---------------------------------------------------------------------------------------------------
        #if NN_KERNEL != 2 && NN_PATH != 2
        // Scalar: 4 threads per row, 16 outputs each, sums in fp32 (fp16 path: each product in fp16).
        void MultiplyBlock( bool transposeWeights, out float acc[16] )
        {
            const uint row  = gl_LocalInvocationIndex >> 2;
            const uint col0 = ( gl_LocalInvocationIndex & 3u ) * 16u;
            for ( uint c = 0; c < 16u; ++c )
                acc[c] = 0.0;
            for ( uint i = 0; i < NN_WIDTH; ++i )
            {
                const nn_t x = sRows[row * NN_WIDTH + i];
                for ( uint c = 0; c < 16u; ++c )
                {
                    const uint o = col0 + c;
                    const nn_t w = transposeWeights ? sWeights[o * NN_WIDTH + i] : sWeights[i * NN_WIDTH + o];
                    acc[c] += float( w * x );
                }
            }
        }
        #endif

        #if NN_KERNEL != 2 && NN_PATH == 2
        // Cooperative matrix: every subgroup takes output tiles in turn; per tile the K loop runs the 64 inputs.
        // The tile loop bound is subgroup-uniform, so every lane reaches every coopmat operation.
        void MultiplyBlockTiles( bool transposeWeights )
        {
            const uint tilesN = NN_WIDTH / NN_TN;
            const uint tiles  = ( NN_BLOCK / NN_TM ) * tilesN;
            for ( uint t = gl_SubgroupID; t < tiles; t += gl_NumSubgroups )
            {
                const uint tm = t / tilesN;
                const uint tn = t % tilesN;
                coopmat<nn_acc_t, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator> acc =
                     coopmat<nn_acc_t, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator>( nn_acc_t( 0.0 ) );
                for ( uint k = 0; k < NN_WIDTH; k += NN_TK )
                {
                    coopmat<float16_t, gl_ScopeSubgroup, NN_TM, NN_TK, gl_MatrixUseA> a;
                    coopMatLoad( a, sRows, tm * NN_TM * NN_WIDTH + k, NN_WIDTH, gl_CooperativeMatrixLayoutRowMajor );
                    coopmat<float16_t, gl_ScopeSubgroup, NN_TK, NN_TN, gl_MatrixUseB> b;
                    // forward  B[k][n] = W[n][k]: W read column-major; backward B[j][k] = W[j][k]: row-major.
                    if ( transposeWeights )
                        coopMatLoad( b, sWeights, tn * NN_TN * NN_WIDTH + k, NN_WIDTH,
                                     gl_CooperativeMatrixLayoutColumnMajor );
                    else
                        coopMatLoad( b, sWeights, k * NN_WIDTH + tn * NN_TN, NN_WIDTH,
                                     gl_CooperativeMatrixLayoutRowMajor );
                    acc = coopMatMulAdd( a, b, acc );
                }
                coopMatStore( acc, sTiles, tm * NN_TM * NN_WIDTH + tn * NN_TN, NN_WIDTH,
                              gl_CooperativeMatrixLayoutRowMajor );
            }
        }
        #endif

        #if NN_KERNEL == 0 || NN_KERNEL == 3
        void main()
        {
            const uint block = gl_WorkGroupID.x * NN_BLOCK;
            for ( uint i = gl_LocalInvocationIndex; i < NN_BLOCK_ELEMENTS; i += NN_THREADS )
            {
                const nn_t h                            = nn_t( Input[block * NN_WIDTH + i] );
                sRows[i]                                = h;
        #if NN_KERNEL == 0
                Activations[RowIndex( 0, block ) + i]   = h;
        #endif
            }

            const uint last = Push.MatrixCount - 1u;
            for ( uint l = 0; l <= last; ++l )
            {
                LoadWeights( l );
                barrier();

        #if NN_PATH == 2
                MultiplyBlockTiles( true );
                barrier();
                for ( uint i = gl_LocalInvocationIndex; i < NN_BLOCK_ELEMENTS; i += NN_THREADS )
                {
                    const float z = float( sTiles[i] );
                    if ( l < last )
                    {
                        const nn_t h                              = nn_t( max( z, 0.0 ) );
                        sRows[i]                                  = h;
        #if NN_KERNEL == 0
                        Activations[RowIndex( l + 1u, block ) + i] = h;
        #endif
                    }
                    else
                        Output[block * NN_WIDTH + i] = z;
                }
        #else
                float acc[16];
                MultiplyBlock( true, acc );
                barrier();
                const uint row  = gl_LocalInvocationIndex >> 2;
                const uint col0 = ( gl_LocalInvocationIndex & 3u ) * 16u;
                for ( uint c = 0; c < 16u; ++c )
                {
                    const uint i = row * NN_WIDTH + col0 + c;
                    if ( l < last )
                    {
                        const nn_t h                              = nn_t( max( acc[c], 0.0 ) );
                        sRows[i]                                  = h;
        #if NN_KERNEL == 0
                        Activations[RowIndex( l + 1u, block ) + i] = h;
        #endif
                    }
                    else
                        Output[block * NN_WIDTH + i] = acc[c];
                }
        #endif
                barrier();
            }
        }
        #endif

        #if NN_KERNEL == 1
        void main()
        {
            const uint block = gl_WorkGroupID.x * NN_BLOCK;
            const uint last  = Push.MatrixCount - 1u;
            for ( uint i = gl_LocalInvocationIndex; i < NN_BLOCK_ELEMENTS; i += NN_THREADS )
            {
                const nn_t g                           = nn_t( OutputGrad[block * NN_WIDTH + i] );
                sRows[i]                               = g;
                Grads[RowIndex( last, block ) + i]     = g;
            }

            for ( uint l = last; l > 0u; --l )
            {
                LoadWeights( l );
                barrier();

        #if NN_PATH == 2
                MultiplyBlockTiles( false );
                barrier();
                for ( uint i = gl_LocalInvocationIndex; i < NN_BLOCK_ELEMENTS; i += NN_THREADS )
                {
                    const bool live                       = float( Activations[RowIndex( l, block ) + i] ) > 0.0;
                    const nn_t g                          = nn_t( live ? float( sTiles[i] ) : 0.0 );
                    sRows[i]                              = g;
                    Grads[RowIndex( l - 1u, block ) + i]  = g;
                }
        #else
                float acc[16];
                MultiplyBlock( false, acc );
                barrier();
                const uint row  = gl_LocalInvocationIndex >> 2;
                const uint col0 = ( gl_LocalInvocationIndex & 3u ) * 16u;
                for ( uint c = 0; c < 16u; ++c )
                {
                    const uint i                          = row * NN_WIDTH + col0 + c;
                    const bool live                       = float( Activations[RowIndex( l, block ) + i] ) > 0.0;
                    const nn_t g                          = nn_t( live ? acc[c] : 0.0 );
                    sRows[i]                              = g;
                    Grads[RowIndex( l - 1u, block ) + i]  = g;
                }
        #endif
                barrier();
            }
        }
        #endif

        // ---------------------------------------------------------------------------------------------------
        // Weight gradient: dW_l[j][k] = sum_b g_l[b][j] * h_l[b][k], one workgroup row of the dispatch per
        // matrix (gl_WorkGroupID.y = l). Each output is summed by ONE invocation (or one tile) in batch order,
        // so the result is the same on every run.
        // ---------------------------------------------------------------------------------------------------
        #if NN_KERNEL == 2 && NN_PATH != 2
        // Scalar: workgroup x owns rows j in [4x, 4x + 4) of the matrix, one (j, k) per invocation; the batch is
        // walked in chunks of NN_BLOCK rows staged in shared memory.
        shared nn_t sGrad[NN_BLOCK * 4];
        shared nn_t sAct[NN_BLOCK_ELEMENTS];

        void main()
        {
            const uint l  = gl_WorkGroupID.y;
            const uint j0 = gl_WorkGroupID.x * 4u;
            const uint jj = gl_LocalInvocationIndex / NN_WIDTH;
            const uint k  = gl_LocalInvocationIndex % NN_WIDTH;

            float acc = 0.0;
            for ( uint chunk = 0; chunk < Push.Batch; chunk += NN_BLOCK )
            {
                for ( uint i = gl_LocalInvocationIndex; i < NN_BLOCK_ELEMENTS; i += NN_THREADS )
                    sAct[i] = Activations[RowIndex( l, chunk ) + i];
                {
                    const uint b = gl_LocalInvocationIndex / 4u;
                    const uint q = gl_LocalInvocationIndex % 4u;
                    sGrad[b * 4u + q] = Grads[RowIndex( l, chunk + b ) + j0 + q];
                }
                barrier();
                for ( uint b = 0; b < NN_BLOCK; ++b )
                    acc += float( sGrad[b * 4u + jj] * sAct[b * NN_WIDTH + k] );
                barrier();
            }
            WeightGrad[l * NN_MATRIX_ELEMENTS + ( j0 + jj ) * NN_WIDTH + k] = acc;
        }
        #endif

        #if NN_KERNEL == 2 && NN_PATH == 2
        // Cooperative matrix: dW_l = G_l^T H_l, (64 x Batch) x (Batch x 64), the K loop over the batch reading
        // straight from the storage buffers: A[j][b] = G[b][j] is G column-major, B[b][k] = H[b][k] row-major.
        void main()
        {
            const uint l      = gl_WorkGroupID.y;
            const uint tilesN = NN_WIDTH / NN_TN;
            const uint tiles  = ( NN_WIDTH / NN_TM ) * tilesN;
            for ( uint t = gl_SubgroupID; t < tiles; t += gl_NumSubgroups )
            {
                const uint tm = t / tilesN;
                const uint tn = t % tilesN;
                coopmat<nn_acc_t, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator> acc =
                     coopmat<nn_acc_t, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator>( nn_acc_t( 0.0 ) );
                for ( uint b = 0; b < Push.Batch; b += NN_TK )
                {
                    coopmat<float16_t, gl_ScopeSubgroup, NN_TM, NN_TK, gl_MatrixUseA> a;
                    coopMatLoad( a, Grads, RowIndex( l, b ) + tm * NN_TM, NN_WIDTH,
                                 gl_CooperativeMatrixLayoutColumnMajor );
                    coopmat<float16_t, gl_ScopeSubgroup, NN_TK, NN_TN, gl_MatrixUseB> h;
                    coopMatLoad( h, Activations, RowIndex( l, b ) + tn * NN_TN, NN_WIDTH,
                                 gl_CooperativeMatrixLayoutRowMajor );
                    acc = coopMatMulAdd( a, h, acc );
                }
                coopmat<float, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator> result =
                     coopmat<float, gl_ScopeSubgroup, NN_TM, NN_TN, gl_MatrixUseAccumulator>( acc );
                coopMatStore( result, WeightGrad, l * NN_MATRIX_ELEMENTS + tm * NN_TM * NN_WIDTH + tn * NN_TN,
                              NN_WIDTH, gl_CooperativeMatrixLayoutRowMajor );
            }
        }
        #endif
    }
}

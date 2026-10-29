#include <Engine/Graphic/Neural/FusedMlpCompute.hpp>

#include <Common/Json/Template.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/gtc/packing.hpp>

#include <cstring>
#include <format>
#include <vector>

namespace Desert::Engine::Neural
{
    namespace
    {
        constexpr const char* kShaderName = "NeuralFusedMlp";
        constexpr uint32_t    W           = kFusedMlpWidth;

        // NN_KERNEL values (NeuralFusedMlp.shader).
        constexpr uint32_t kForward    = 0;
        constexpr uint32_t kBackward   = 1;
        constexpr uint32_t kWeightGrad = 2;

        // The shader's bindings (NeuralFusedMlp.shader: 0..2 read, 3..6 written).
        enum Binding : uint32_t
        {
            kWeights     = 0,
            kInput       = 1,
            kOutputGrad  = 2,
            kOutput      = 3,
            kActivations = 4,
            kGrads       = 5,
            kWeightGrads = 6,
        };

        struct NeuralMlpPush
        {
            uint32_t Batch       = 0;
            uint32_t MatrixCount = 0;
        };

        uint32_t PathDefine( const NeuralKernelPath path )
        {
            switch ( path )
            {
                case NeuralKernelPath::Fp32:
                    return 0;
                case NeuralKernelPath::Fp16:
                    return 1;
                case NeuralKernelPath::CoopMatFp16:
                    return 2;
            }
            return 0;
        }

        // Bytes of one activation / gradient element: the fp16 paths store h_l and g_l as fp16.
        uint32_t StoredElementBytes( const NeuralKernelPath path )
        {
            return path == NeuralKernelPath::Fp32 ? 4u : 2u;
        }

        std::vector<float> FromStored( const std::vector<uint8_t>& bytes, const NeuralKernelPath path )
        {
            std::vector<float> values( bytes.size() / StoredElementBytes( path ) );
            if ( path == NeuralKernelPath::Fp32 )
            {
                std::memcpy( values.data(), bytes.data(), values.size() * sizeof( float ) );
                return values;
            }
            for ( size_t i = 0; i < values.size(); ++i )
            {
                uint16_t half = 0;
                std::memcpy( &half, bytes.data() + i * 2, 2 );
                values[i] = glm::unpackHalf1x16( half );
            }
            return values;
        }
    } // namespace

    Common::ResultStr<NeuralKernelPath> ResolveNeuralKernelPath( const NeuralComputeCaps&              caps,
                                                                 const std::optional<NeuralKernelPath> forced )
    {
        if ( !forced )
            return Common::MakeSuccess( NeuralKernelPath{ caps.Path } );

        switch ( *forced )
        {
            case NeuralKernelPath::Fp32:
                return Common::MakeSuccess( NeuralKernelPath{ NeuralKernelPath::Fp32 } );

            case NeuralKernelPath::Fp16:
            {
                std::vector<std::string> missing;
                if ( !caps.ShaderFloat16 )
                    missing.emplace_back( "shaderFloat16" );
                if ( !caps.StorageBuffer16BitAccess )
                    missing.emplace_back( "storageBuffer16BitAccess" );
                if ( !missing.empty() )
                    return Common::MakeFormattedError<NeuralKernelPath>(
                         "the forced NN kernel path fp16 is refused: the device lacks {}",
                         Common::Text::Join( missing, ", " ) );
                return Common::MakeSuccess( NeuralKernelPath{ NeuralKernelPath::Fp16 } );
            }

            case NeuralKernelPath::CoopMatFp16:
                if ( caps.Path != NeuralKernelPath::CoopMatFp16 )
                    return Common::MakeFormattedError<NeuralKernelPath>(
                         "the forced NN kernel path coopmat_fp16 is refused: {}", caps.Reason );
                return Common::MakeSuccess( NeuralKernelPath{ NeuralKernelPath::CoopMatFp16 } );
        }
        return Common::MakeFormattedError<NeuralKernelPath>( "unknown NN kernel path {}",
                                                             static_cast<uint32_t>( *forced ) );
    }

    FusedMlpCompute::FusedMlpCompute( const NeuralComputeCaps& caps ) : m_Caps( caps )
    {
    }

    FusedMlpCompute::~FusedMlpCompute() = default;

    Common::ResultStr<std::shared_ptr<Graphic::ComputePipeline>>
    FusedMlpCompute::Pipeline( const uint32_t kernel, const NeuralKernelPath path )
    {
        using PipelineResult = std::shared_ptr<Graphic::ComputePipeline>;

        ::Desert::Core::ShaderVariant variant;
        variant.Defines = { std::format( "NN_KERNEL={}", kernel ),
                            std::format( "NN_PATH={}", PathDefine( path ) ) };
        if ( path == NeuralKernelPath::CoopMatFp16 )
        {
            // The tile is the device's (SelectNeuralKernelPath), so these variants are per device: not cooked.
            const CooperativeMatrixShape& tile = m_Caps.Tile;
            if ( tile.M == 0 || tile.N == 0 || tile.K == 0 || W % tile.M != 0 || W % tile.N != 0 ||
                 W % tile.K != 0 )
                return Common::MakeFormattedError<PipelineResult>(
                     "the cooperative-matrix tile {} does not divide the network width {}",
                     FormatCooperativeMatrixShape( tile ), W );
            variant.Defines.push_back( std::format( "NN_TM={}", tile.M ) );
            variant.Defines.push_back( std::format( "NN_TN={}", tile.N ) );
            variant.Defines.push_back( std::format( "NN_TK={}", tile.K ) );
            if ( tile.C == NeuralScalar::Float16 )
                variant.Defines.emplace_back( "NN_ACC_FP16" );
        }

        const std::string key = Common::Text::Join( variant.Defines, " " );
        if ( const auto found = m_Pipelines.find( key ); found != m_Pipelines.end() )
            return Common::MakeSuccess( PipelineResult{ found->second } );

        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
            return Common::MakeFormattedError<PipelineResult>( "no shader service to compile '{}' ({})",
                                                               kShaderName, key );
        const std::shared_ptr<Graphic::Shader> shader = service->AcquireVariant( kShaderName, variant );
        if ( !shader )
            return Common::MakeFormattedError<PipelineResult>(
                 "the shader '{}' ({}) did not compile — expected "
                 "Editor/Resources/Shaders/Programs/Compute/NeuralFusedMlp.shader",
                 kShaderName, key );
        const auto built = Graphic::ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::format( "{} ({})", kShaderName, key ) } );
        if ( !built )
            return Common::MakeFormattedError<PipelineResult>( "pipeline '{}' ({}): {}", kShaderName, key,
                                                               built.GetError() );
        m_Pipelines[key] = built.GetValue();
        return Common::MakeSuccess( PipelineResult{ built.GetValue() } );
    }

    Common::ResultStr<FusedMlpResult> FusedMlpCompute::Run( const FusedMlpProblem& problem,
                                                            const NeuralKernelPath path )
    {
        if ( const auto resolved = ResolveNeuralKernelPath( m_Caps, path ); !resolved )
            return Common::MakeError<FusedMlpResult>( resolved.GetError() );
        if ( const std::string invalid = ValidateFusedMlp( problem.Shape, problem.Batch ); !invalid.empty() )
            return Common::MakeFormattedError<FusedMlpResult>( "fused MLP problem refused: {}", invalid );

        const uint32_t L     = problem.Shape.MatrixCount();
        const uint32_t batch = problem.Batch;
        if ( problem.Weights.size() != size_t( L ) * W * W || problem.Input.size() != size_t( batch ) * W ||
             problem.OutputGrad.size() != size_t( batch ) * W )
            return Common::MakeFormattedError<FusedMlpResult>(
                 "fused MLP problem refused: weights {} / input {} / output grad {} floats, the shape needs {} / "
                 "{} / {}",
                 problem.Weights.size(), problem.Input.size(), problem.OutputGrad.size(), size_t( L ) * W * W,
                 size_t( batch ) * W, size_t( batch ) * W );

        std::shared_ptr<Graphic::ComputePipeline> kernels[3];
        for ( const uint32_t kernel : { kForward, kBackward, kWeightGrad } )
        {
            auto pipeline = Pipeline( kernel, path );
            if ( !pipeline )
                return Common::MakeFormattedError<FusedMlpResult>(
                     "NN {} kernel {}: {}", NeuralKernelPathName( path ), kernel, pipeline.GetError() );
            kernels[kernel] = pipeline.GetValue();
        }

        // Per-frame buffers (host-visible copies of this frame index): every Dispatch below waits on its fence,
        // so the copies read back at the end hold this run's writes.
        const uint32_t stored        = StoredElementBytes( path );
        const uint32_t floatBytes    = sizeof( float );
        const uint32_t ioBytes       = batch * W * floatBytes;
        const uint32_t weightBytes   = L * W * W * floatBytes;
        const uint32_t perLayerBytes = L * batch * W * stored;

        struct BufferSpec
        {
            Binding     Slot;
            const char* Name;
            uint32_t    Bytes;
        };
        const BufferSpec specs[] = {
             { kWeights, "NeuralFusedMlp.Weights", weightBytes },
             { kInput, "NeuralFusedMlp.Input", ioBytes },
             { kOutputGrad, "NeuralFusedMlp.OutputGrad", ioBytes },
             { kOutput, "NeuralFusedMlp.Output", ioBytes },
             { kActivations, "NeuralFusedMlp.Activations", perLayerBytes },
             { kGrads, "NeuralFusedMlp.Grads", perLayerBytes },
             { kWeightGrads, "NeuralFusedMlp.WeightGrad", weightBytes },
        };
        std::shared_ptr<ShaderResources::StorageBuffer> buffers[7];
        for ( const BufferSpec& spec : specs )
        {
            buffers[spec.Slot] = ShaderResources::StorageBuffer::Create( spec.Name, spec.Bytes, spec.Slot );
            if ( !buffers[spec.Slot] )
                return Common::MakeFormattedError<FusedMlpResult>(
                     "storage buffer '{}' ({} bytes) was not created", spec.Name, spec.Bytes );
        }

        const std::pair<Binding, const std::vector<float>*> uploads[] = {
             { kWeights, &problem.Weights }, { kInput, &problem.Input }, { kOutputGrad, &problem.OutputGrad } };
        for ( const auto& [slot, values] : uploads )
            if ( const auto wrote =
                      buffers[slot]->SetData( values->data(), uint32_t( values->size() * floatBytes ) );
                 !wrote )
                return Common::MakeFormattedError<FusedMlpResult>(
                     "NN {} upload: {}", NeuralKernelPathName( path ), wrote.GetError() );

        const NeuralMlpPush push{ .Batch = batch, .MatrixCount = L };
        const uint32_t      blocks = batch / kFusedMlpBatchGranularity;

        // Forward: reads weights + input, writes the output and every h_l.
        kernels[kForward]
             ->SetStorageBuffer( kWeights, buffers[kWeights].get() )
             .SetStorageBuffer( kInput, buffers[kInput].get() )
             .SetStorageBuffer( kOutput, buffers[kOutput].get() )
             .SetStorageBuffer( kActivations, buffers[kActivations].get() )
             .SetPushConstants( &push, sizeof( push ) )
             .Dispatch( blocks, 1, 1 );

        // Backward: reads weights, dL/dy and every h_l, writes every g_l.
        kernels[kBackward]
             ->SetStorageBuffer( kWeights, buffers[kWeights].get() )
             .SetStorageBuffer( kOutputGrad, buffers[kOutputGrad].get() )
             .SetStorageBuffer( kActivations, buffers[kActivations].get() )
             .SetStorageBuffer( kGrads, buffers[kGrads].get() )
             .SetPushConstants( &push, sizeof( push ) )
             .Dispatch( blocks, 1, 1 );

        // Weight gradient: dW_l = sum_b g_l (x) h_l. Scalar: 16 groups of 4 rows per matrix; coopmat: one group
        // per matrix, its subgroups walking the tiles.
        uint32_t weightGradGroups = W / 4u;
        if ( path == NeuralKernelPath::CoopMatFp16 )
            weightGradGroups = 1u;
        kernels[kWeightGrad]
             ->SetStorageBuffer( kActivations, buffers[kActivations].get() )
             .SetStorageBuffer( kGrads, buffers[kGrads].get() )
             .SetStorageBuffer( kWeightGrads, buffers[kWeightGrads].get() )
             .SetPushConstants( &push, sizeof( push ) )
             .Dispatch( weightGradGroups, L, 1 );

        FusedMlpResult result;
        result.Output.resize( size_t( batch ) * W );
        result.WeightGrad.resize( size_t( L ) * W * W );
        std::vector<uint8_t> activations( perLayerBytes );
        if ( const auto read = buffers[kOutput]->ReadBack( result.Output.data(), ioBytes ); !read )
            return Common::MakeFormattedError<FusedMlpResult>( "NN {} output read-back: {}",
                                                               NeuralKernelPathName( path ), read.GetError() );
        if ( const auto read = buffers[kWeightGrads]->ReadBack( result.WeightGrad.data(), weightBytes ); !read )
            return Common::MakeFormattedError<FusedMlpResult>( "NN {} weight-gradient read-back: {}",
                                                               NeuralKernelPathName( path ), read.GetError() );
        if ( const auto read = buffers[kActivations]->ReadBack( activations.data(), perLayerBytes ); !read )
            return Common::MakeFormattedError<FusedMlpResult>( "NN {} activation read-back: {}",
                                                               NeuralKernelPathName( path ), read.GetError() );
        result.Activations = FromStored( activations, path );
        return Common::MakeSuccess( std::move( result ) );
    }

    Common::ResultStr<FusedMlpPathCheck>
    CheckFusedMlpPath( FusedMlpCompute& compute, const NeuralComputeCaps& caps, const NeuralKernelPath path )
    {
        const FusedMlpProblem problem =
             MakeSeededFusedMlpProblem( kFusedMlpCheckShape, kFusedMlpCheckBatch, kFusedMlpCheckSeed );
        const auto gpu = compute.Run( problem, path );
        if ( !gpu )
            return Common::MakeError<FusedMlpPathCheck>( gpu.GetError() );

        // The fp16 networks hold fp16 weights and inputs: their reference is the exact MLP of those values.
        const FusedMlpResult reference =
             RunFusedMlpReference( path == NeuralKernelPath::Fp32 ? problem : QuantizedToFp16( problem ) );

        FusedMlpPathCheck check;
        check.Path = path;
        if ( path == NeuralKernelPath::CoopMatFp16 )
            check.Tile = FormatCooperativeMatrixShape( caps.Tile );
        check.Output     = CompareFusedMlp( gpu.GetValue().Output, reference.Output );
        check.WeightGrad = CompareFusedMlp( gpu.GetValue().WeightGrad, reference.WeightGrad );
        check.MaskFlips =
             CountReluMaskFlips( gpu.GetValue().Activations, reference.Activations, problem.Shape, problem.Batch );
        check.Tolerance = FusedMlpTolerance( path );
        check.Pass      = check.Output.Rel <= check.Tolerance && check.WeightGrad.Rel <= check.Tolerance;
        return Common::MakeSuccess( std::move( check ) );
    }
} // namespace Desert::Engine::Neural

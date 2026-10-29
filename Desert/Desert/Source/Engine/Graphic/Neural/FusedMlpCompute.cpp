#include <Engine/Graphic/Neural/FusedMlpCompute.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/Template.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <glm/gtc/packing.hpp>

#include <cstring>
#include <format>
#include <span>
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

        // The shader's slots (NeuralFusedMlp.shader), spelled once. Each kernel variant declares only the ones it
        // touches: forward NnWeights NnInput -> NnOutput NnActivations; backward NnWeights NnOutputGrad
        // NnActivations -> NnGrads; weight gradient NnActivations NnGrads -> NnWeightGrad.
        struct NeuralFusedMlpShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, NnWeights );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnInput );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnOutputGrad );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnOutput );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnActivations );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnGrads );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnWeightGrad );
        };
        using Slots = NeuralFusedMlpShaderParameters;

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

        std::vector<float> FromStored( const std::vector<uint8_t>& bytes, const NeuralKernelPath path )
        {
            std::vector<float> values( bytes.size() / FusedMlpStoredElementBytes( path ) );
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

    uint32_t FusedMlpStoredElementBytes( const NeuralKernelPath path )
    {
        return path == NeuralKernelPath::Fp32 ? 4u : 2u;
    }

    Common::ResultStr<NeuralSubgroupRequest> CoopMatSubgroupRequest( const NeuralComputeCaps& caps )
    {
        const uint32_t width = caps.SubgroupSize;
        if ( width == 0 || kFusedMlpCoopMatThreads % width != 0 )
            return Common::MakeFormattedError<NeuralSubgroupRequest>(
                 "the device's subgroup width {} does not divide the coopmat workgroup of {} invocations", width,
                 kFusedMlpCoopMatThreads );

        NeuralSubgroupRequest request;
        if ( !caps.SubgroupSizeControl )
        {
            // Without subgroupSizeControl + computeFullSubgroups the device offers no control and reports no
            // range: it runs compute at subgroupSize, and a workgroup that is a multiple of it is what it gets.
            request.Reason = std::format( "no subgroup size control: the device's subgroupSize {} divides the "
                                          "{}-invocation workgroup",
                                          width, kFusedMlpCoopMatThreads );
            return Common::MakeSuccess( std::move( request ) );
        }

        request.RequireFullSubgroups = true;
        if ( caps.MinSubgroupSize == caps.MaxSubgroupSize )
        {
            request.Reason = std::format( "full subgroups; the width is fixed at {}", width );
            return Common::MakeSuccess( std::move( request ) );
        }
        if ( !caps.ComputeSubgroupSizeRequirable )
            return Common::MakeFormattedError<NeuralSubgroupRequest>(
                 "the device varies its subgroup width ({}..{}) and cannot require one for compute "
                 "(requiredSubgroupSizeStages lacks COMPUTE); its coopmat shapes are listed for width {}",
                 caps.MinSubgroupSize, caps.MaxSubgroupSize, width );
        request.RequiredSubgroupSize = width;
        request.Reason               = std::format(
             "full subgroups of the required width {} (the device varies {}..{}; its coopmat shapes are listed for "
             "subgroupSize {})",
             width, caps.MinSubgroupSize, caps.MaxSubgroupSize, width );
        return Common::MakeSuccess( std::move( request ) );
    }

    FusedMlpCompute::FusedMlpCompute( const NeuralComputeCaps& caps ) : m_Caps( caps )
    {
    }

    FusedMlpCompute::~FusedMlpCompute() = default;

    Common::ResultStr<FusedMlpCompute::Kernel*> FusedMlpCompute::AcquireKernel( const uint32_t         kernel,
                                                                               const NeuralKernelPath path )
    {
        using KernelResult = Kernel*;

        ::Desert::Core::ShaderVariant variant;
        variant.Defines = { std::format( "NN_KERNEL={}", kernel ), std::format( "NN_PATH={}", PathDefine( path ) ) };
        NeuralSubgroupRequest subgroups;
        if ( path == NeuralKernelPath::CoopMatFp16 )
        {
            // The tile is the device's (SelectNeuralKernelPath), so these variants are per device: not cooked.
            const CooperativeMatrixShape& tile = m_Caps.Tile;
            if ( tile.M == 0 || tile.N == 0 || tile.K == 0 || W % tile.M != 0 || W % tile.N != 0 ||
                 W % tile.K != 0 )
                return Common::MakeFormattedError<KernelResult>(
                     "the cooperative-matrix tile {} does not divide the network width {}",
                     FormatCooperativeMatrixShape( tile ), W );
            variant.Defines.push_back( std::format( "NN_TM={}", tile.M ) );
            variant.Defines.push_back( std::format( "NN_TN={}", tile.N ) );
            variant.Defines.push_back( std::format( "NN_TK={}", tile.K ) );
            if ( tile.C == NeuralScalar::Float16 )
                variant.Defines.emplace_back( "NN_ACC_FP16" );

            auto requested = CoopMatSubgroupRequest( m_Caps );
            if ( !requested )
                return Common::MakeFormattedError<KernelResult>( "the coopmat_fp16 kernels are refused: {}",
                                                                 requested.GetError() );
            subgroups = requested.GetValue();
        }

        const std::string key = Common::Text::Join( variant.Defines, " " );
        if ( const auto found = m_Kernels.find( key ); found != m_Kernels.end() )
            return Common::MakeSuccess( KernelResult{ &found->second } );

        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
            return Common::MakeFormattedError<KernelResult>( "no shader service to compile '{}' ({})", kShaderName,
                                                             key );
        auto acquired = service->AcquireVariant( kShaderName, variant );
        if ( !acquired )
            return Common::MakeFormattedError<KernelResult>(
                 "the shader '{}' ({}) was refused: {} (source "
                 "Editor/Resources/Shaders/Programs/Compute/NeuralFusedMlp.shader)",
                 kShaderName, key, acquired.GetError() );
        const std::shared_ptr<Graphic::Shader> shader = acquired.ExtractValue();

        Graphic::ComputePipelineSpecification specification;
        specification.Shader               = shader;
        specification.DebugName            = std::format( "{} ({})", kShaderName, key );
        specification.RequiredSubgroupSize = subgroups.RequiredSubgroupSize;
        specification.RequireFullSubgroups = subgroups.RequireFullSubgroups;
        const auto built                   = Graphic::ComputePipeline::Create( specification );
        if ( !built )
            return Common::MakeFormattedError<KernelResult>( "pipeline '{}' ({}): {}", kShaderName, key,
                                                             built.GetError() );

        // The slots the variant declares must be the ones its pass binds: a shader edit that renames or drops
        // one is refused here, by name, not as a validation fault of every graph.
        std::vector<Graphic::RDG::ShaderParameter> expected;
        switch ( kernel )
        {
            case kForward:
                expected = { Graphic::RDG::Erase( Slots::NnWeights ), Graphic::RDG::Erase( Slots::NnInput ),
                             Graphic::RDG::Erase( Slots::NnOutput ), Graphic::RDG::Erase( Slots::NnActivations ) };
                break;
            case kBackward:
                expected = { Graphic::RDG::Erase( Slots::NnWeights ), Graphic::RDG::Erase( Slots::NnOutputGrad ),
                             Graphic::RDG::Erase( Slots::NnActivations ), Graphic::RDG::Erase( Slots::NnGrads ) };
                break;
            default:
                expected = { Graphic::RDG::Erase( Slots::NnActivations ), Graphic::RDG::Erase( Slots::NnGrads ),
                             Graphic::RDG::Erase( Slots::NnWeightGrad ) };
                break;
        }
        Kernel made;
        made.Pipeline = built.GetValue();
        if ( const auto valid = made.Layout.Validate( shader, expected ); !valid )
            return Common::MakeFormattedError<KernelResult>( "pipeline '{}' ({}): {}", kShaderName, key,
                                                             valid.GetError() );

        if ( path == NeuralKernelPath::CoopMatFp16 )
            LOG_INFO( "[nn_pipeline] {} ({}): requireFullSubgroups={} requiredSubgroupSize={} — {}", kShaderName,
                      key, subgroups.RequireFullSubgroups, subgroups.RequiredSubgroupSize, subgroups.Reason );
        Kernel& kept = m_Kernels[key] = std::move( made );
        return Common::MakeSuccess( KernelResult{ &kept } );
    }

    Common::ResultStr<FusedMlpGraphOutputs> FusedMlpCompute::AddTrainingPasses( Graphic::RDG::Builder&     graph,
                                                                               const FusedMlpGraphInputs& inputs,
                                                                               const NeuralKernelPath     path )
    {
        namespace RDG = Graphic::RDG;

        if ( const auto resolved = ResolveNeuralKernelPath( m_Caps, path ); !resolved )
            return Common::MakeError<FusedMlpGraphOutputs>( resolved.GetError() );
        if ( const std::string invalid = ValidateFusedMlp( inputs.Shape, inputs.Batch ); !invalid.empty() )
            return Common::MakeFormattedError<FusedMlpGraphOutputs>( "fused MLP problem refused: {}", invalid );
        if ( !inputs.Weights.IsValid() || !inputs.Input.IsValid() || !inputs.OutputGrad.IsValid() )
            return Common::MakeFormattedError<FusedMlpGraphOutputs>(
                 "fused MLP passes refused: an input handle is invalid (weights {}, input {}, output grad {})",
                 inputs.Weights.IsValid(), inputs.Input.IsValid(), inputs.OutputGrad.IsValid() );

        Kernel* kernels[3] = {};
        for ( const uint32_t kernel : { kForward, kBackward, kWeightGrad } )
        {
            auto acquired = AcquireKernel( kernel, path );
            if ( !acquired )
                return Common::MakeFormattedError<FusedMlpGraphOutputs>(
                     "NN {} kernel {}: {}", NeuralKernelPathName( path ), kernel, acquired.GetError() );
            kernels[kernel] = acquired.GetValue();
        }

        const uint32_t L             = inputs.Shape.MatrixCount();
        const uint32_t batch         = inputs.Batch;
        const uint64_t ioBytes       = uint64_t( batch ) * W * sizeof( float );
        const uint64_t weightBytes   = uint64_t( L ) * W * W * sizeof( float );
        const uint64_t perLayerBytes = uint64_t( L ) * batch * W * FusedMlpStoredElementBytes( path );

        FusedMlpGraphOutputs out;
        out.Output      = graph.CreateBuffer( RDG::BufferDesc{ ioBytes }, "NeuralFusedMlp.Output" );
        out.Activations = graph.CreateBuffer( RDG::BufferDesc{ perLayerBytes }, "NeuralFusedMlp.Activations" );
        out.Grads       = graph.CreateBuffer( RDG::BufferDesc{ perLayerBytes }, "NeuralFusedMlp.Grads" );
        out.WeightGrad  = graph.CreateBuffer( RDG::BufferDesc{ weightBytes }, "NeuralFusedMlp.WeightGrad" );

        const NeuralMlpPush push{ .Batch = batch, .MatrixCount = L };
        const uint32_t      blocks = batch / kFusedMlpBatchGranularity;
        // Weight gradient: scalar 16 groups of 4 rows per matrix; coopmat one group per matrix, its subgroups
        // walking the tiles.
        const uint32_t weightGradGroups = path == NeuralKernelPath::CoopMatFp16 ? 1u : W / 4u;

        // The exec of one pass: its block's bindings, the push, one dispatch. Barriers are the graph's.
        const auto record = []( std::shared_ptr<Graphic::ComputePipeline> pipeline, const NeuralMlpPush values,
                                const uint32_t groupsX, const uint32_t groupsY )
        {
            return [pipeline = std::move( pipeline ), values, groupsX,
                    groupsY]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                bindings.PushConstants( &values, sizeof( values ) );
                return Graphic::Renderer::DispatchCompute( bindings, *pipeline, groupsX, groupsY, 1 );
            };
        };

        Kernel& forward    = *kernels[kForward];
        Kernel& backward   = *kernels[kBackward];
        Kernel& weightGrad = *kernels[kWeightGrad];

        // Forward: h_0 = input, h_{l+1} = ReLU(W_l h_l), y = W_last h_last.
        graph.AddPass(
             "NN: Forward", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( forward.Layout.Get( forward.Pipeline->GetSpecification().Shader ),
                                Graphic::Renderer::GetPipelineRouteFill( *forward.Pipeline ) )
                      .Storage( Slots::NnWeights, inputs.Weights, RDG::Access::StorageRead )
                      .Storage( Slots::NnInput, inputs.Input, RDG::Access::StorageRead )
                      .Storage( Slots::NnOutput, out.Output, RDG::Access::StorageWrite )
                      .Storage( Slots::NnActivations, out.Activations, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( NeuralMlpPush ) ) );
             },
             record( forward.Pipeline, push, blocks, 1 ) );

        // Backward: g_last = dL/dy, g_{l-1} = (W_l^T g_l) * [h_l > 0], after the forward wrote every h_l.
        graph.AddPass(
             "NN: Backward", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( backward.Layout.Get( backward.Pipeline->GetSpecification().Shader ),
                                Graphic::Renderer::GetPipelineRouteFill( *backward.Pipeline ) )
                      .Storage( Slots::NnWeights, inputs.Weights, RDG::Access::StorageRead )
                      .Storage( Slots::NnOutputGrad, inputs.OutputGrad, RDG::Access::StorageRead )
                      .Storage( Slots::NnActivations, out.Activations, RDG::Access::StorageRead )
                      .Storage( Slots::NnGrads, out.Grads, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( NeuralMlpPush ) ) );
             },
             record( backward.Pipeline, push, blocks, 1 ) );

        // Weight gradient: dW_l = sum_b g_l (x) h_l, after the backward wrote every g_l.
        graph.AddPass(
             "NN: Weight gradient", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( weightGrad.Layout.Get( weightGrad.Pipeline->GetSpecification().Shader ),
                                Graphic::Renderer::GetPipelineRouteFill( *weightGrad.Pipeline ) )
                      .Storage( Slots::NnActivations, out.Activations, RDG::Access::StorageRead )
                      .Storage( Slots::NnGrads, out.Grads, RDG::Access::StorageRead )
                      .Storage( Slots::NnWeightGrad, out.WeightGrad, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( NeuralMlpPush ) ) );
             },
             record( weightGrad.Pipeline, push, weightGradGroups, L ) );

        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<std::unique_ptr<FusedMlpCheckRun>>
    FusedMlpCheckRun::Submit( FusedMlpCompute& compute, const NeuralComputeCaps& caps, const NeuralKernelPath path )
    {
        namespace RDG   = Graphic::RDG;
        using RunResult = std::unique_ptr<FusedMlpCheckRun>;

        std::unique_ptr<FusedMlpCheckRun> run( new FusedMlpCheckRun() );
        run->m_Path    = path;
        run->m_Problem = MakeSeededFusedMlpProblem( kFusedMlpCheckShape, kFusedMlpCheckBatch, kFusedMlpCheckSeed );
        if ( path == NeuralKernelPath::CoopMatFp16 )
            run->m_Tile = FormatCooperativeMatrixShape( caps.Tile );
        const FusedMlpProblem& problem = run->m_Problem;

        RDG::Builder graph( "NeuralFusedMlpCheck" );
        const auto   upload = [&graph]( const std::vector<float>& values, const char* name )
        {
            const RDG::BufferRef buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ uint64_t( values.size() ) * sizeof( float ) }, name );
            graph.QueueBufferUpload( buffer, std::as_bytes( std::span( values ) ) );
            return buffer;
        };
        FusedMlpGraphInputs inputs;
        inputs.Weights    = upload( problem.Weights, "NeuralFusedMlp.Weights" );
        inputs.Input      = upload( problem.Input, "NeuralFusedMlp.Input" );
        inputs.OutputGrad = upload( problem.OutputGrad, "NeuralFusedMlp.OutputGrad" );
        inputs.Shape      = problem.Shape;
        inputs.Batch      = problem.Batch;
        const auto added  = compute.AddTrainingPasses( graph, inputs, path );
        if ( !added )
            return Common::MakeError<RunResult>( added.GetError() );

        // HostRead: the graph ends each on a compute -> host barrier into host-visible memory, and ReadForHost
        // invalidates it before handing the bytes over: nothing here assumes coherent memory.
        graph.Extract( added.GetValue().Output, run->m_Output, RDG::Access::HostRead );
        graph.Extract( added.GetValue().WeightGrad, run->m_WeightGrad, RDG::Access::HostRead );
        graph.Extract( added.GetValue().Activations, run->m_Activations, RDG::Access::HostRead );

        run->m_SubmittedFrame = FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( const auto executed = Graphic::Renderer::ExecuteGraph( graph ); !executed )
            return Common::MakeFormattedError<RunResult>( "NN {} graph: {}", NeuralKernelPathName( path ),
                                                          executed.GetError() );
        const RDG::ExecuteReport& report = graph.GetExecuteReport();
        if ( !report.Faults.empty() )
        {
            std::vector<std::string> faults;
            for ( const RDG::PassFault& fault : report.Faults )
                faults.push_back( std::format( "'{}' {}: {}", fault.PassName,
                                               RDG::GetPassFaultStageName( fault.Stage ), fault.Reason ) );
            return Common::MakeFormattedError<RunResult>( "NN {} graph faulted: {}", NeuralKernelPathName( path ),
                                                          Common::Text::Join( faults, "; " ) );
        }
        if ( !run->m_Output.Physical || !run->m_WeightGrad.Physical || !run->m_Activations.Physical )
            return Common::MakeFormattedError<RunResult>(
                 "NN {} graph executed but handed back no read-back memory", NeuralKernelPathName( path ) );
        return Common::MakeSuccess( std::move( run ) );
    }

    bool FusedMlpCheckRun::IsComplete() const
    {
        const FrameManager& frames = FrameManager::GetInstance();
        return frames.GetAbsoluteFrameCount() >= m_SubmittedFrame + frames.GetMaxFramesInFlight();
    }

    Common::ResultStr<FusedMlpPathCheck> FusedMlpCheckRun::Complete() const
    {
        const char* name = NeuralKernelPathName( m_Path );
        if ( !IsComplete() )
            return Common::MakeFormattedError<FusedMlpPathCheck>(
                 "NN {} read-back asked before its frame completed (submitted at frame {}, now {})", name,
                 m_SubmittedFrame, FrameManager::GetInstance().GetAbsoluteFrameCount() );

        const FusedMlpProblem& problem = m_Problem;
        const uint32_t         L       = problem.Shape.MatrixCount();
        const auto             bytesOf = [name]( const Graphic::RDG::ExternalBuffer& buffer, const size_t needed,
                                     const char* what ) -> Common::ResultStr<std::vector<uint8_t>>
        {
            const std::span<const std::byte> bytes = buffer.Physical->ReadForHost();
            if ( bytes.size() < needed )
                return Common::MakeFormattedError<std::vector<uint8_t>>(
                     "NN {} {} read-back holds {} bytes, the shape needs {}", name, what, bytes.size(), needed );
            std::vector<uint8_t> copy( needed );
            std::memcpy( copy.data(), bytes.data(), needed );
            return Common::MakeSuccess( std::move( copy ) );
        };

        FusedMlpResult gpu;
        gpu.Output.resize( size_t( problem.Batch ) * W );
        gpu.WeightGrad.resize( size_t( L ) * W * W );
        const auto output = bytesOf( m_Output, gpu.Output.size() * sizeof( float ), "output" );
        if ( !output )
            return Common::MakeError<FusedMlpPathCheck>( output.GetError() );
        std::memcpy( gpu.Output.data(), output.GetValue().data(), output.GetValue().size() );
        const auto weightGrad = bytesOf( m_WeightGrad, gpu.WeightGrad.size() * sizeof( float ), "weight-gradient" );
        if ( !weightGrad )
            return Common::MakeError<FusedMlpPathCheck>( weightGrad.GetError() );
        std::memcpy( gpu.WeightGrad.data(), weightGrad.GetValue().data(), weightGrad.GetValue().size() );
        const auto activations = bytesOf(
             m_Activations, size_t( L ) * problem.Batch * W * FusedMlpStoredElementBytes( m_Path ), "activation" );
        if ( !activations )
            return Common::MakeError<FusedMlpPathCheck>( activations.GetError() );
        gpu.Activations = FromStored( activations.GetValue(), m_Path );

        // The fp16 networks hold fp16 weights and inputs: their reference is the exact MLP of those values.
        const FusedMlpResult reference =
             RunFusedMlpReference( m_Path == NeuralKernelPath::Fp32 ? problem : QuantizedToFp16( problem ) );

        FusedMlpPathCheck check;
        check.Path       = m_Path;
        check.Tile       = m_Tile;
        check.Output     = CompareFusedMlp( gpu.Output, reference.Output );
        check.WeightGrad = CompareFusedMlp( gpu.WeightGrad, reference.WeightGrad );
        check.MaskFlips  = CountReluMaskFlips( gpu.Activations, reference.Activations, problem.Shape, problem.Batch );
        check.Tolerance         = FusedMlpTolerance( m_Path );
        check.GradientTolerance = FusedMlpGradientTolerance( m_Path, problem.Batch );
        check.Pass = check.Output.Rel <= check.Tolerance && check.WeightGrad.Rel <= check.GradientTolerance;
        return Common::MakeSuccess( std::move( check ) );
    }
} // namespace Desert::Engine::Neural

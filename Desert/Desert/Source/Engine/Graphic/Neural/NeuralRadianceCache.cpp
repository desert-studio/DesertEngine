#include <Engine/Graphic/Neural/NeuralRadianceCache.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <span>

namespace Desert::Engine::Neural
{
    namespace
    {
        constexpr const char* kShaderName = "NeuralRadianceCache";
        constexpr uint32_t    W           = kFusedMlpWidth;
        constexpr uint32_t    kNoCounter  = 0xFFFFFFFFu;
        constexpr uint32_t    kGroup      = 64; // LocalSize of every NeuralRadianceCache.shader pass

        constexpr uint32_t kWeightElements = kNrcMatrixCount * W * W;
        constexpr uint32_t kHashElements   = kNrcHashLevels * kNrcHashTableSize * kNrcHashFeatures;
        constexpr uint32_t kLossPartials   = kNrcTrainingSteps * ( kNrcTrainingBatch / kGroup ) * 2;

        // Record strides in uvec4 (Encode's Rows.x): NrcQuery, NrcTrainingSample, NrcVertex.
        constexpr uint32_t kQueryStride    = sizeof( NrcQueryGpu ) / 16;
        constexpr uint32_t kTrainingStride = sizeof( NrcTrainingSampleGpu ) / 16;
        constexpr uint32_t kVertexStride   = sizeof( NrcVertexGpu ) / 16;

        // The shader's slots (NeuralRadianceCache.shader), spelled once.
        struct NeuralRadianceCacheShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcSourceUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcHashTableUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcCountersUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnInput );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcTrainingSamplesUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcSelfQueriesUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcSelfOutputUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnOutput );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnOutputGrad );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcLossPartialsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnWeights );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnGrads );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcHashGradUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NnWeightGrad );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcWeightsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcAdamMlpUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcAdamHashUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcQueriesUB );
            DESERT_SHADER_PARAMETER( StorageTexture, AccumulationTexture );
            DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
            DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
        };
        using Slots = NeuralRadianceCacheShaderParameters;

        // NeuralRadianceCache.shader "NrcPush".
        struct NrcPush
        {
            glm::uvec4 Rows{ 0u };
            glm::uvec4 Extra{ 0u };
            glm::vec4  Adam{ 0.0f };
            glm::vec4  Adam2{ 0.0f };
        };

        uint32_t Groups( const uint64_t rows )
        {
            return static_cast<uint32_t>( ( rows + kGroup - 1 ) / kGroup );
        }

        // splitmix64: the hash table's initial values, the same on every machine.
        uint64_t SplitMix( uint64_t& state )
        {
            uint64_t z = ( state += 0x9E3779B97F4A7C15ull );
            z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
            z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
            return z ^ ( z >> 31 );
        }

        // The exec of one NeuralRadianceCache.shader pass: its push and one dispatch.
        RDG::Builder::ExecFunction RecordDispatch( std::shared_ptr<Graphic::ComputePipeline> pipeline,
                                                   const NrcPush push, const uint32_t groupsX,
                                                   const uint32_t groupsY )
        {
            return [pipeline = std::move( pipeline ), push, groupsX,
                    groupsY]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                return Graphic::Renderer::DispatchCompute( bindings, *pipeline, groupsX, groupsY, 1 );
            };
        }

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
    } // namespace

    NeuralRadianceCache::NeuralRadianceCache( const NeuralComputeCaps& caps ) : m_Caps( caps )
    {
    }

    NeuralRadianceCache::~NeuralRadianceCache() = default;

    Common::BoolResultStr NeuralRadianceCache::Initialize()
    {
        const auto resolved =
             ResolveNeuralKernelPath( m_Caps, Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath );
        if ( !resolved )
            return Common::MakeFormattedError( "NRC: the NN layer's kernel path is refused: {}",
                                               resolved.GetError() );
        m_Path = resolved.GetValue();
        m_Mlp  = std::make_unique<FusedMlpCompute>( m_Caps );

        Runtime::ShaderService* shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaders )
            return Common::MakeError( "NRC: no shader service" );

        // Each program with the slots its pass binds: a shader edit that renames or drops one is refused here.
        using Graphic::RDG::Erase;
        const std::pair<Program*, std::vector<Graphic::RDG::ShaderParameter>> programs[] = {
             { &m_Encode,
               { Erase( Slots::NrcSourceUB ), Erase( Slots::NrcHashTableUB ), Erase( Slots::NrcCountersUB ),
                 Erase( Slots::NnInput ) } },
             { &m_Loss,
               { Erase( Slots::NrcTrainingSamplesUB ), Erase( Slots::NrcSelfQueriesUB ),
                 Erase( Slots::NrcSelfOutputUB ), Erase( Slots::NnOutput ), Erase( Slots::NrcCountersUB ),
                 Erase( Slots::NnOutputGrad ), Erase( Slots::NrcLossPartialsUB ) } },
             { &m_InputGrad,
               { Erase( Slots::NnWeights ), Erase( Slots::NnGrads ), Erase( Slots::NrcTrainingSamplesUB ),
                 Erase( Slots::NrcCountersUB ), Erase( Slots::NrcHashGradUB ) } },
             { &m_AdamMlp,
               { Erase( Slots::NnWeightGrad ), Erase( Slots::NrcWeightsUB ), Erase( Slots::NrcAdamMlpUB ),
                 Erase( Slots::NrcCountersUB ) } },
             { &m_AdamHash,
               { Erase( Slots::NrcHashTableUB ), Erase( Slots::NrcAdamHashUB ), Erase( Slots::NrcHashGradUB ),
                 Erase( Slots::NrcCountersUB ) } },
             { &m_Composite,
               { Erase( Slots::NrcQueriesUB ), Erase( Slots::NnOutput ), Erase( Slots::AccumulationTexture ),
                 Erase( Slots::DemodulatedDiffuseTexture ), Erase( Slots::DemodulatedSpecularTexture ) } },
        };
        for ( const auto& [program, expected] : programs )
        {
            const std::string name = std::format( "{}/{}", kShaderName, program->Pass );
            // InputGrad reads the MLP's gradients in the path's storage type (NN_PATH, as NeuralFusedMlp.shader).
            if ( program == &m_InputGrad && m_Path != NeuralKernelPath::Fp32 )
            {
                ::Desert::Core::ShaderVariant variant;
                variant.Defines = { std::format( "NN_PATH={}", PathDefine( m_Path ) ) };
                program->Shader = shaders->AcquireVariant( name, variant );
            }
            else
                program->Shader = shaders->GetByName( name );
            if ( !program->Shader )
                return Common::MakeFormattedError(
                     "NRC: the program '{}' is not registered or did not compile — expected "
                     "Editor/Resources/Shaders/Programs/Compute/NeuralRadianceCache.shader",
                     name );
            const auto pipeline = Graphic::ComputePipeline::Create(
                 { .Shader    = program->Shader,
                   .DebugName = std::format( "NeuralRadianceCache{}", program->Pass ) } );
            if ( !pipeline )
                return Common::MakeFormattedError( "NRC: pipeline '{}': {}", name, pipeline.GetError() );
            program->Pipeline = pipeline.GetValue();
            if ( const auto valid = program->Layout.Validate( program->Shader, expected ); !valid )
                return Common::MakeFormattedError( "NRC: pipeline '{}': {}", name, valid.GetError() );
        }

        const auto& frames = FrameManager::GetInstance();
        m_ReadBacks.assign( frames.GetMaxFramesInFlight() + 1u, ReadBack{} );
        m_Stats.KernelPath = NeuralKernelPathName( m_Path );
        m_Initialized      = true;
        return BOOLSUCCESS;
    }

    void NeuralRadianceCache::Reset()
    {
        m_Persistent.Built = false;
        m_AdamStep         = 0;
        for ( ReadBack& slot : m_ReadBacks )
            slot.Pending = false;
        const std::string path = m_Stats.KernelPath;
        m_Stats                = NrcStats{};
        m_Stats.KernelPath     = path;
    }

    NeuralRadianceCache::PersistentRefs NeuralRadianceCache::AcquirePersistent( RDG::Builder& graph )
    {
        if ( m_Persistent.Built )
            return { .Weights   = graph.RegisterExternal( m_Persistent.Weights, "NRC.Weights" ),
                     .AdamMlp   = graph.RegisterExternal( m_Persistent.AdamMlp, "NRC.AdamMlp" ),
                     .HashTable = graph.RegisterExternal( m_Persistent.HashTable, "NRC.HashTable" ),
                     .AdamHash  = graph.RegisterExternal( m_Persistent.AdamHash, "NRC.AdamHash" ),
                     .HashGrad  = graph.RegisterExternal( m_Persistent.HashGrad, "NRC.HashGrad" ) };

        // A fresh cache: He-uniform MLP weights (FusedMlp.hpp's seeded initialiser, zero outside the shape), the
        // hash table uniform in +-kNrcHashInitScale (instant-ngp), every moment and gradient zero.
        const std::vector<float> weights =
             MakeSeededFusedMlpProblem( kNrcShape, kFusedMlpBatchGranularity, 0x4E52432D57ull /* "NRC-W" */ )
                  .Weights;
        std::vector<float> table( kHashElements );
        uint64_t           state = 0x4E52432D48ull; // "NRC-H"
        for ( float& value : table )
            value = ( float( SplitMix( state ) >> 40 ) / float( 1ull << 24 ) * 2.0f - 1.0f ) * kNrcHashInitScale;
        const std::vector<float>   adamMlp( 2u * kWeightElements, 0.0f );
        const std::vector<float>   adamHash( 2u * kHashElements, 0.0f );
        const std::vector<int32_t> hashGrad( kHashElements, 0 );

        const auto create = [&graph]( const auto& values, const char* name )
        {
            const RDG::BufferRef buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ uint64_t( values.size() ) * sizeof( values[0] ) }, name );
            graph.QueueBufferUpload( buffer, std::as_bytes( std::span( values ) ) );
            return buffer;
        };
        const PersistentRefs refs{ .Weights   = create( weights, "NRC.Weights" ),
                                   .AdamMlp   = create( adamMlp, "NRC.AdamMlp" ),
                                   .HashTable = create( table, "NRC.HashTable" ),
                                   .AdamHash  = create( adamHash, "NRC.AdamHash" ),
                                   .HashGrad  = create( hashGrad, "NRC.HashGrad" ) };
        graph.Extract( refs.Weights, m_Persistent.Weights, RDG::Access::StorageWrite );
        graph.Extract( refs.AdamMlp, m_Persistent.AdamMlp, RDG::Access::StorageWrite );
        graph.Extract( refs.HashTable, m_Persistent.HashTable, RDG::Access::StorageWrite );
        graph.Extract( refs.AdamHash, m_Persistent.AdamHash, RDG::Access::StorageWrite );
        graph.Extract( refs.HashGrad, m_Persistent.HashGrad, RDG::Access::StorageWrite );
        m_Persistent.Built = true;
        return refs;
    }

    void NeuralRadianceCache::CollectReadBacks()
    {
        const FrameManager& frames = FrameManager::GetInstance();
        // Oldest first, so the history keeps the frames' order.
        std::vector<ReadBack*> ready;
        for ( ReadBack& slot : m_ReadBacks )
            if ( slot.Pending && frames.GetAbsoluteFrameCount() >= slot.Frame + frames.GetMaxFramesInFlight() )
                ready.push_back( &slot );
        std::sort( ready.begin(), ready.end(),
                   []( const ReadBack* a, const ReadBack* b ) { return a->Frame < b->Frame; } );
        for ( ReadBack* slot : ready )
        {
            slot->Pending = false;
            if ( !slot->Counters.Physical || !slot->LossPartials.Physical )
                continue;
            const std::span<const std::byte> counters = slot->Counters.Physical->ReadForHost();
            const std::span<const std::byte> partials = slot->LossPartials.Physical->ReadForHost();
            if ( counters.size() < 4 * sizeof( uint32_t ) || partials.size() < kLossPartials * sizeof( float ) )
            {
                m_Stats.Error = std::format( "NRC read-back of frame {} is short ({} / {} bytes)", slot->Frame,
                                             counters.size(), partials.size() );
                continue;
            }
            uint32_t count[4] = {};
            std::memcpy( count, counters.data(), sizeof( count ) );
            std::vector<float> loss( kLossPartials );
            std::memcpy( loss.data(), partials.data(), kLossPartials * sizeof( float ) );
            double sum = 0.0, rows = 0.0;
            for ( uint32_t i = 0; i < kLossPartials; i += 2 )
            {
                sum += loss[i];
                rows += loss[i + 1];
            }
            m_Stats.TrainingSamples =
                 std::min( count[uint32_t( NrcCounter::TrainingSamples )], kNrcTrainingSamples );
            m_Stats.SelfQueries   = std::min( count[uint32_t( NrcCounter::SelfQueries )], kNrcMaxTrainingPaths );
            m_Stats.TrainingPaths = count[uint32_t( NrcCounter::TrainingPaths )];
            m_Stats.RenderQueries = count[uint32_t( NrcCounter::RenderQueries )];
            if ( rows <= 0.0 )
                continue; // a frame that trained on nothing has no loss to report
            m_Stats.TrainLoss = float( sum / rows );
            if ( m_Stats.LossHistory.empty() )
                m_Stats.FirstTrainLoss = m_Stats.TrainLoss;
            m_Stats.LossHistory.push_back( m_Stats.TrainLoss );
            if ( m_Stats.LossHistory.size() > kNrcLossHistory )
                m_Stats.LossHistory.erase( m_Stats.LossHistory.begin() );
        }
    }

    Common::ResultStr<NrcTraceTargets>
    NeuralRadianceCache::AddTraceTargets( RDG::Builder& graph, const uint32_t width, const uint32_t height,
                                          const uint32_t pathsPerFrame, const uint32_t frameSeed )
    {
        if ( !m_Initialized )
            return Common::MakeFormattedError<NrcTraceTargets>( "NRC: Initialize was not called or refused" );
        if ( width == 0u || height == 0u || pathsPerFrame == 0u )
            return Common::MakeFormattedError<NrcTraceTargets>( "NRC: no paths to cache ({} x {} x {})", width,
                                                                height, pathsPerFrame );
        CollectReadBacks();

        NrcTraceTargets targets;
        targets.Width          = width;
        targets.Height         = height;
        targets.PathsPerFrame  = pathsPerFrame;
        const uint64_t queries = uint64_t( width ) * height * pathsPerFrame;
        targets.Queries = graph.CreateBuffer( RDG::BufferDesc{ queries * sizeof( NrcQueryGpu ) }, "NRC.Queries" );
        targets.TrainingSamples = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kNrcTrainingSamples ) * sizeof( NrcTrainingSampleGpu ) },
             "NRC.TrainingSamples" );
        targets.SelfQueries = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kNrcMaxTrainingPaths ) * sizeof( NrcVertexGpu ) }, "NRC.SelfQueries" );
        targets.Counters = graph.CreateBuffer( RDG::BufferDesc{ 4 * sizeof( uint32_t ) }, "NRC.Counters" );
        static constexpr uint32_t kZero[4] = {};
        graph.QueueBufferUpload( targets.Counters, std::as_bytes( std::span( kZero ) ) );

        // One training pixel per tile, about kNrcMaxTrainingPaths of them; the pixel inside the tile moves every
        // frame (the paper's random offset), from a hash of the frame seed.
        const double   pixels = double( width ) * height;
        const uint32_t tile  = std::max( 1u, uint32_t( std::ceil( std::sqrt( pixels / kNrcMaxTrainingPaths ) ) ) );
        uint64_t       state = frameSeed;
        const uint64_t offset = SplitMix( state );
        targets.Push.Params =
             glm::uvec4( 1u, tile, uint32_t( offset % tile ), uint32_t( ( offset >> 32 ) % tile ) );
        targets.Push.Spread =
             glm::vec4( kNrcSpreadThreshold, float( kNrcUnbiasedTrainingPaths ), kNrcMinThroughput, 0.0f );
        return Common::MakeSuccess( std::move( targets ) );
    }

    RDG::BufferRef NeuralRadianceCache::AddEncodePass( RDG::Builder& graph, const PersistentRefs& cache,
                                                       const RDG::BufferRef source, const uint32_t strideVec4,
                                                       const uint32_t firstRow, const uint32_t rows,
                                                       const RDG::BufferRef counters, const uint32_t countIndex )
    {
        // The records a row may encode: the counter's appended rows (capped at its capacity) for the training
        // samples and the self queries, every query of the chunk for the render queries. Rows past them are zero.
        uint32_t records = firstRow + rows, capacity = 0;
        if ( countIndex == uint32_t( NrcCounter::TrainingSamples ) )
            records = capacity = kNrcTrainingSamples;
        else if ( countIndex == uint32_t( NrcCounter::SelfQueries ) )
            records = capacity = kNrcMaxTrainingPaths;
        const uint32_t       dispatched = Groups( rows ) * kGroup;
        const RDG::BufferRef input      = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( dispatched ) * W * sizeof( float ) }, "NRC.Input" );
        NrcPush push;
        push.Rows  = glm::uvec4( strideVec4, firstRow, dispatched, records );
        push.Extra = glm::uvec4( countIndex, capacity, 0u, 0u );
        graph.AddPass(
             "NRC: Encode", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_Encode.Layout.Get( m_Encode.Shader ),
                                Graphic::Renderer::GetPipelineRouteFill( *m_Encode.Pipeline ) )
                      .Storage( Slots::NrcSourceUB, source, RDG::Access::StorageRead )
                      .Storage( Slots::NrcHashTableUB, cache.HashTable, RDG::Access::StorageRead )
                      .Storage( Slots::NrcCountersUB, counters, RDG::Access::StorageRead )
                      .Storage( Slots::NnInput, input, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
             },
             RecordDispatch( m_Encode.Pipeline, push, Groups( dispatched ), 1 ) );
        return input;
    }

    Common::BoolResultStr NeuralRadianceCache::AddCachePasses( RDG::Builder& graph, const NrcTraceTargets& trace,
                                                               const NrcCompositeTargets& composite )
    {
        if ( !m_Initialized )
            return Common::MakeError( "NRC: Initialize was not called or refused" );
        const PersistentRefs cache = AcquirePersistent( graph );
        const auto           mlpInputs =
             [&]( const RDG::BufferRef input, const uint32_t batch, const RDG::BufferRef outputGrad )
        {
            return FusedMlpGraphInputs{ .Weights    = cache.Weights,
                                        .Input      = input,
                                        .OutputGrad = outputGrad,
                                        .Shape      = kNrcShape,
                                        .Batch      = batch };
        };

        // ---- Inference of this frame's render queries, chunk by chunk, each composited into the sums. ----
        const uint32_t pixels = trace.Width * trace.Height;
        const uint32_t total  = pixels * trace.PathsPerFrame;
        // At most one query of a pixel per chunk (Composite writes its pixel without atomics).
        const uint32_t chunk = std::max( kGroup, std::min( kNrcInferenceChunk, pixels ) / kGroup * kGroup );
        for ( uint32_t first = 0; first < total; first += chunk )
        {
            const uint32_t       rows  = std::min( chunk, total - first );
            const RDG::BufferRef input = AddEncodePass( graph, cache, trace.Queries, kQueryStride, first, rows,
                                                        trace.Counters, kNoCounter );
            const auto           output =
                 m_Mlp->AddInferencePass( graph, mlpInputs( input, Groups( rows ) * kGroup, {} ), m_Path );
            if ( !output )
                return Common::MakeFormattedError( "NRC inference: {}", output.GetError() );
            NrcPush push;
            push.Rows = glm::uvec4( first, rows, trace.Width, trace.Height );
            graph.AddPass(
                 "NRC: Composite", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Composite.Layout.Get( m_Composite.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_Composite.Pipeline ) )
                          .Storage( Slots::NrcQueriesUB, trace.Queries, RDG::Access::StorageRead )
                          .Storage( Slots::NnOutput, output.GetValue(), RDG::Access::StorageRead )
                          .Storage( Slots::AccumulationTexture, composite.Radiance, RDG::Access::StorageWrite )
                          .Storage( Slots::DemodulatedDiffuseTexture, composite.DemodulatedDiffuse,
                                    RDG::Access::StorageWrite )
                          .Storage( Slots::DemodulatedSpecularTexture, composite.DemodulatedSpecular,
                                    RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_Composite.Pipeline, push, Groups( rows ), 1 ) );
        }

        // ---- The cache at the self-training suffix ends, before this frame's training. ----
        const RDG::BufferRef selfInput =
             AddEncodePass( graph, cache, trace.SelfQueries, kVertexStride, 0, kNrcMaxTrainingPaths,
                            trace.Counters, uint32_t( NrcCounter::SelfQueries ) );
        const auto selfOutput =
             m_Mlp->AddInferencePass( graph, mlpInputs( selfInput, kNrcMaxTrainingPaths, {} ), m_Path );
        if ( !selfOutput )
            return Common::MakeFormattedError( "NRC self-training inference: {}", selfOutput.GetError() );

        // ---- Training: kNrcTrainingSteps Adam steps of kNrcTrainingBatch rows. ----
        const RDG::BufferRef lossPartials = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kLossPartials ) * sizeof( float ) }, "NRC.LossPartials" );
        for ( uint32_t step = 0; step < kNrcTrainingSteps; ++step )
        {
            const RDG::BufferRef input =
                 AddEncodePass( graph, cache, trace.TrainingSamples, kTrainingStride, step * kNrcTrainingBatch,
                                kNrcTrainingBatch, trace.Counters, uint32_t( NrcCounter::TrainingSamples ) );
            const auto forward = m_Mlp->AddForwardPass( graph, mlpInputs( input, kNrcTrainingBatch, {} ), m_Path );
            if ( !forward )
                return Common::MakeFormattedError( "NRC training forward: {}", forward.GetError() );

            const RDG::BufferRef outputGrad = graph.CreateBuffer(
                 RDG::BufferDesc{ uint64_t( kNrcTrainingBatch ) * W * sizeof( float ) }, "NRC.OutputGrad" );
            NrcPush lossPush;
            lossPush.Rows = glm::uvec4( step, kNrcTrainingBatch, kNrcMaxTrainingPaths, 0u );
            graph.AddPass(
                 "NRC: Loss", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Loss.Layout.Get( m_Loss.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_Loss.Pipeline ) )
                          .Storage( Slots::NrcTrainingSamplesUB, trace.TrainingSamples, RDG::Access::StorageRead )
                          .Storage( Slots::NrcSelfQueriesUB, trace.SelfQueries, RDG::Access::StorageRead )
                          .Storage( Slots::NrcSelfOutputUB, selfOutput.GetValue(), RDG::Access::StorageRead )
                          .Storage( Slots::NnOutput, forward.GetValue().Output, RDG::Access::StorageRead )
                          .Storage( Slots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .Storage( Slots::NnOutputGrad, outputGrad, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcLossPartialsUB, lossPartials, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_Loss.Pipeline, lossPush, Groups( kNrcTrainingBatch ), 1 ) );

            const auto backward = m_Mlp->AddBackwardPasses(
                 graph, mlpInputs( input, kNrcTrainingBatch, outputGrad ), forward.GetValue(), m_Path );
            if ( !backward )
                return Common::MakeFormattedError( "NRC training backward: {}", backward.GetError() );

            NrcPush gradPush;
            gradPush.Rows = glm::uvec4( step, kNrcTrainingBatch, 0u, 0u );
            graph.AddPass(
                 "NRC: Input gradient", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_InputGrad.Layout.Get( m_InputGrad.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_InputGrad.Pipeline ) )
                          .Storage( Slots::NnWeights, cache.Weights, RDG::Access::StorageRead )
                          .Storage( Slots::NnGrads, backward.GetValue().Grads, RDG::Access::StorageRead )
                          .Storage( Slots::NrcTrainingSamplesUB, trace.TrainingSamples, RDG::Access::StorageRead )
                          .Storage( Slots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .Storage( Slots::NrcHashGradUB, cache.HashGrad, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_InputGrad.Pipeline, gradPush, Groups( kNrcTrainingBatch ), 1 ) );

            const uint32_t t = static_cast<uint32_t>( ++m_AdamStep );
            NrcPush        mlpPush;
            mlpPush.Rows  = glm::uvec4( step, kNrcTrainingBatch, kWeightElements, t );
            mlpPush.Adam  = glm::vec4( kNrcLearningRate, kNrcAdamBeta1, kNrcAdamBeta2, kNrcAdamEpsilon );
            mlpPush.Adam2 = glm::vec4( kNrcWeightDecay, 0.0f, 0.0f, 0.0f );
            graph.AddPass(
                 "NRC: Adam MLP", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_AdamMlp.Layout.Get( m_AdamMlp.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_AdamMlp.Pipeline ) )
                          .Storage( Slots::NnWeightGrad, backward.GetValue().WeightGrad, RDG::Access::StorageRead )
                          .Storage( Slots::NrcWeightsUB, cache.Weights, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcAdamMlpUB, cache.AdamMlp, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_AdamMlp.Pipeline, mlpPush, Groups( kWeightElements ), 1 ) );

            NrcPush hashPush = mlpPush;
            hashPush.Rows.z  = kHashElements;
            hashPush.Adam2   = glm::vec4( 0.0f );
            // 2D: 1024 groups of 64 per row of the dispatch (the shader's e = x + y * 65536).
            constexpr uint32_t kHashGroupsX = 1024;
            graph.AddPass(
                 "NRC: Adam Hash", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_AdamHash.Layout.Get( m_AdamHash.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_AdamHash.Pipeline ) )
                          .Storage( Slots::NrcHashTableUB, cache.HashTable, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcAdamHashUB, cache.AdamHash, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcHashGradUB, cache.HashGrad, RDG::Access::StorageWrite )
                          .Storage( Slots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_AdamHash.Pipeline, hashPush, kHashGroupsX,
                                 ( kHashElements + kHashGroupsX * kGroup - 1 ) / ( kHashGroupsX * kGroup ) ) );
        }
        m_Stats.TrainingSteps = m_AdamStep;
        m_Stats.Active        = true;

        // ---- The stats read-back: counters and loss partials, read once the frame's fence passed. ----
        const uint64_t frame = FrameManager::GetInstance().GetAbsoluteFrameCount();
        ReadBack&      slot  = m_ReadBacks[frame % m_ReadBacks.size()];
        slot.Frame           = frame;
        slot.Pending         = true;
        graph.Extract( trace.Counters, slot.Counters, RDG::Access::HostRead );
        graph.Extract( lossPartials, slot.LossPartials, RDG::Access::HostRead );
        return BOOLSUCCESS;
    }
} // namespace Desert::Engine::Neural

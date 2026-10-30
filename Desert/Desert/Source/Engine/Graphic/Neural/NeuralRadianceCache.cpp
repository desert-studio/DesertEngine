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
        constexpr const char* kNrcShader  = NeuralRadianceCache::kProgram;
        constexpr uint32_t    kNoCounter  = 0xFFFFFFFFu;
        constexpr uint32_t    kGroup      = 64; // LocalSize of every NeuralRadianceCache.shader pass

        constexpr uint32_t kWeightElements = kNrcMatrixCount * kFusedMlpWidth * kFusedMlpWidth;
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
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcWeightsEmaUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcInferenceWeightsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, NrcHashEmaUB );
            DESERT_SHADER_PARAMETER( StorageTexture, AccumulationTexture );
            DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
            DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
        };
        using NrcSlots = NeuralRadianceCacheShaderParameters;

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
    } // namespace

    NeuralRadianceCache::NeuralRadianceCache( const NeuralComputeCaps& caps ) : m_Caps( caps )
    {
    }

    NeuralRadianceCache::~NeuralRadianceCache() = default;

    Common::BoolResultStr NeuralRadianceCache::Initialize()
    {
        m_ShaderRefusal.reset();
        const auto resolved =
             ResolveNeuralKernelPath( m_Caps, Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath );
        if ( !resolved )
            return Common::MakeFormattedError( "NRC: the NN layer's kernel path is refused: {}",
                                               resolved.GetError() );
        m_Path = resolved.GetValue();
        // Every MLP batch a frame runs (NeuralRadianceCache.hpp FRAME ORDER: an inference chunk - the query
        // capacity is whole chunks, so every chunk is full -, the self-training inference, a training step): a
        // shape or batch the kernels do not run is refused here, before any frame adds a pass, not halfway
        // through AddCachePasses.
        for ( const uint32_t batch :
              { Groups( kNrcInferenceChunk ) * kGroup, kNrcMaxTrainingPaths, kNrcTrainingBatch } )
            if ( const std::string invalid = ValidateFusedMlp( kNrcShape, batch ); !invalid.empty() )
                return Common::MakeFormattedError( "NRC: the MLP batch of {} rows is refused: {}", batch,
                                                   invalid );
        m_Mlp = std::make_unique<FusedMlpCompute>( m_Caps );
        // The MLP's kernels now: a refusal stops the cache before the frame's graph has any of its passes.
        if ( const auto kernels = m_Mlp->PrepareKernels( m_Path ); !kernels )
        {
            m_ShaderRefusal = m_Mlp->GetShaderRefusal();
            return Common::MakeFormattedError( "NRC: the NN layer's {} kernels are refused: {}",
                                               NeuralKernelPathName( m_Path ), kernels.GetError() );
        }

        Runtime::ShaderService* shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaders )
            return Common::MakeError( "NRC: no shader service" );

        // Each program with the slots its pass binds: a shader edit that renames or drops one is refused here.
        using Graphic::RDG::Erase;
        const std::pair<Program*, std::vector<Graphic::RDG::ShaderParameter>> programs[] = {
             { &m_Encode,
               { Erase( NrcSlots::NrcSourceUB ), Erase( NrcSlots::NrcHashTableUB ),
                 Erase( NrcSlots::NrcCountersUB ), Erase( NrcSlots::NnInput ) } },
             { &m_Loss,
               { Erase( NrcSlots::NrcTrainingSamplesUB ), Erase( NrcSlots::NrcSelfQueriesUB ),
                 Erase( NrcSlots::NrcSelfOutputUB ), Erase( NrcSlots::NnOutput ), Erase( NrcSlots::NrcCountersUB ),
                 Erase( NrcSlots::NnOutputGrad ), Erase( NrcSlots::NrcLossPartialsUB ) } },
             { &m_InputGrad,
               { Erase( NrcSlots::NnWeights ), Erase( NrcSlots::NnGrads ), Erase( NrcSlots::NrcTrainingSamplesUB ),
                 Erase( NrcSlots::NrcCountersUB ), Erase( NrcSlots::NrcHashGradUB ) } },
             { &m_AdamMlp,
               { Erase( NrcSlots::NnWeightGrad ), Erase( NrcSlots::NrcWeightsUB ), Erase( NrcSlots::NrcAdamMlpUB ),
                 Erase( NrcSlots::NrcCountersUB ), Erase( NrcSlots::NrcWeightsEmaUB ),
                 Erase( NrcSlots::NrcInferenceWeightsUB ) } },
             { &m_AdamHash,
               { Erase( NrcSlots::NrcHashTableUB ), Erase( NrcSlots::NrcAdamHashUB ),
                 Erase( NrcSlots::NrcHashGradUB ), Erase( NrcSlots::NrcCountersUB ),
                 Erase( NrcSlots::NrcHashEmaUB ) } },
             { &m_Composite,
               { Erase( NrcSlots::NrcQueriesUB ), Erase( NrcSlots::NnOutput ),
                 Erase( NrcSlots::AccumulationTexture ), Erase( NrcSlots::DemodulatedDiffuseTexture ),
                 Erase( NrcSlots::DemodulatedSpecularTexture ), Erase( NrcSlots::NrcCountersUB ) } },
        };
        for ( const auto& [program, expected] : programs )
        {
            const std::string name = std::format( "{}/{}", kNrcShader, program->Pass );
            // InputGrad reads the MLP's gradients in the path's storage type (NN_PATH, as NeuralFusedMlp.shader).
            if ( program == &m_InputGrad && m_Path != NeuralKernelPath::Fp32 )
            {
                ::Desert::Core::ShaderVariant variant;
                variant.Defines = { std::format( "NN_PATH={}", NeuralKernelPathDefine( m_Path ) ) };
                auto acquired = shaders->AcquireVariant( name, variant );
                if ( !acquired )
                {
                    m_ShaderRefusal = NeuralShaderRefusal{ .Program = name, .Variant = variant };
                    return Common::MakeFormattedError( "NRC: the program '{}' ({}) was refused: {}", name,
                                                       variant.Defines.front(), acquired.GetError() );
                }
                program->Shader = acquired.ExtractValue();
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
        m_QueryCapacity    = 0;
        m_QueryDemand.clear();
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
                     .HashGrad  = graph.RegisterExternal( m_Persistent.HashGrad, "NRC.HashGrad" ),
                     .WeightsEma = graph.RegisterExternal( m_Persistent.WeightsEma, "NRC.WeightsEma" ),
                     .InferenceWeights =
                          graph.RegisterExternal( m_Persistent.InferenceWeights, "NRC.InferenceWeights" ),
                     .HashEma = graph.RegisterExternal( m_Persistent.HashEma, "NRC.HashEma" ) };

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
        const std::vector<float>   weightsEma( kWeightElements, 0.0f );
        const std::vector<float>   hashEma( kHashElements, 0.0f );

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
                                   .HashGrad  = create( hashGrad, "NRC.HashGrad" ),
                                   .WeightsEma       = create( weightsEma, "NRC.WeightsEma" ),
                                   .InferenceWeights = create( weights, "NRC.InferenceWeights" ),
                                   .HashEma          = create( hashEma, "NRC.HashEma" ) };
        graph.Extract( refs.Weights, m_Persistent.Weights, RDG::Access::StorageWrite );
        graph.Extract( refs.AdamMlp, m_Persistent.AdamMlp, RDG::Access::StorageWrite );
        graph.Extract( refs.HashTable, m_Persistent.HashTable, RDG::Access::StorageWrite );
        graph.Extract( refs.AdamHash, m_Persistent.AdamHash, RDG::Access::StorageWrite );
        graph.Extract( refs.HashGrad, m_Persistent.HashGrad, RDG::Access::StorageWrite );
        graph.Extract( refs.WeightsEma, m_Persistent.WeightsEma, RDG::Access::StorageWrite );
        graph.Extract( refs.InferenceWeights, m_Persistent.InferenceWeights, RDG::Access::StorageWrite );
        graph.Extract( refs.HashEma, m_Persistent.HashEma, RDG::Access::StorageWrite );
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
            // THE QUERIES: this frame's demand per segment into the window that sizes the capacity.
            m_QueryDemand.push_back( ( m_Stats.RenderQueries + slot->Paths - 1u ) / std::max( slot->Paths, 1u ) );
            if ( m_QueryDemand.size() > kNrcQueryDemandWindow )
                m_QueryDemand.erase( m_QueryDemand.begin() );
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
        // THE QUERIES: the capacity per segment from the demand window (the start share before a read-back), in
        // whole inference chunks, at most a chunk-rounded pixel count.
        const uint64_t pixelCount = uint64_t( width ) * height;
        const auto     chunks     = []( const double rows )
        {
            return uint32_t( ( uint64_t( std::ceil( rows ) ) + kNrcInferenceChunk - 1 ) / kNrcInferenceChunk *
                             kNrcInferenceChunk );
        };
        const uint32_t ceiling = std::max( kNrcInferenceChunk, chunks( double( pixelCount ) ) );
        if ( m_QueryDemand.empty() )
            m_QueryCapacity = m_QueryCapacity ? m_QueryCapacity : chunks( double( pixelCount ) * kNrcQueryStartShare );
        else
            m_QueryCapacity = chunks( double( *std::max_element( m_QueryDemand.begin(), m_QueryDemand.end() ) ) *
                                      kNrcQueryHeadroom );
        m_QueryCapacity        = std::clamp( m_QueryCapacity, kNrcInferenceChunk, ceiling );
        targets.QueryCapacity  = m_QueryCapacity;
        const uint64_t queries = uint64_t( m_QueryCapacity ) * pathsPerFrame;
        targets.Queries = graph.CreateBuffer( RDG::BufferDesc{ queries * sizeof( NrcQueryGpu ) }, "NRC.Queries" );
        m_Stats.QueryCapacity    = m_QueryCapacity;
        m_Stats.QueryMemoryBytes =
             queries * sizeof( NrcQueryGpu ) +
             2ull * std::min( kNrcInferenceChunk, m_QueryCapacity ) * kFusedMlpWidth * sizeof( float );
        targets.TrainingSamples = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kNrcTrainingSamples ) * sizeof( NrcTrainingSampleGpu ) },
             "NRC.TrainingSamples" );
        targets.SelfQueries = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kNrcMaxTrainingPaths ) * sizeof( NrcVertexGpu ) }, "NRC.SelfQueries" );
        const std::vector<uint32_t> zero( uint32_t( NrcCounter::Segments ) + pathsPerFrame, 0u );
        targets.Counters =
             graph.CreateBuffer( RDG::BufferDesc{ zero.size() * sizeof( uint32_t ) }, "NRC.Counters" );
        graph.QueueBufferUpload( targets.Counters, std::as_bytes( std::span( zero ) ) );

        // One training pixel per tile, about kNrcMaxTrainingPaths of them; the pixel inside the tile moves every
        // frame (the paper's random offset), from a hash of the frame seed.
        const double   pixels = double( width ) * height;
        const uint32_t tile  = std::max( 1u, uint32_t( std::ceil( std::sqrt( pixels / kNrcMaxTrainingPaths ) ) ) );
        uint64_t       state = frameSeed;
        const uint64_t offset = SplitMix( state );
        NrcTraceParams params;
        params.Params =
             glm::uvec4( m_QueryCapacity, tile, uint32_t( offset % tile ), uint32_t( ( offset >> 32 ) % tile ) );
        params.Spread =
             glm::vec4( kNrcSpreadThreshold, float( kNrcUnbiasedTrainingPaths ), kNrcMinThroughput, 0.0f );
        targets.Params = graph.CreateBuffer( RDG::BufferDesc{ sizeof( NrcTraceParams ) }, "NRC.TraceParams" );
        graph.QueueBufferUpload( targets.Params, std::as_bytes( std::span( &params, 1 ) ) );
        return Common::MakeSuccess( std::move( targets ) );
    }

    RDG::BufferRef NeuralRadianceCache::AddEncodePass( RDG::Builder& graph, const RDG::BufferRef hashTable,
                                                       const float hashScale,
                                                       const RDG::BufferRef source, const uint32_t strideVec4,
                                                       const uint32_t firstRow, const uint32_t rows,
                                                       const RDG::BufferRef counters, const uint32_t countIndex,
                                                       uint32_t capacity, const uint32_t countBase )
    {
        // The records a row may encode: the counter's appended rows (capped at its capacity) for the training
        // samples and the self queries, every query of the chunk for the render queries. Rows past them are zero.
        uint32_t records = firstRow + rows;
        if ( countIndex == uint32_t( NrcCounter::TrainingSamples ) )
            records = capacity = kNrcTrainingSamples;
        else if ( countIndex == uint32_t( NrcCounter::SelfQueries ) )
            records = capacity = kNrcMaxTrainingPaths;
        const uint32_t       dispatched = Groups( rows ) * kGroup;
        const RDG::BufferRef input      = graph.CreateBuffer(
             RDG::BufferDesc{ static_cast<uint64_t>( dispatched ) * kFusedMlpWidth * sizeof( float ) },
             "NRC.Input" );
        NrcPush push;
        push.Rows  = glm::uvec4( strideVec4, firstRow, dispatched, records );
        push.Extra = glm::uvec4( countIndex, capacity, countBase, 0u );
        push.Adam  = glm::vec4( hashScale, 0.0f, 0.0f, 0.0f );
        graph.AddPass(
             "NRC: Encode", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_Encode.Layout.Get( m_Encode.Shader ),
                                Graphic::Renderer::GetPipelineRouteFill( *m_Encode.Pipeline ) )
                      .Storage( NrcSlots::NrcSourceUB, source, RDG::Access::StorageRead )
                      .Storage( NrcSlots::NrcHashTableUB, hashTable, RDG::Access::StorageRead )
                      .Storage( NrcSlots::NrcCountersUB, counters, RDG::Access::StorageRead )
                      .Storage( NrcSlots::NnInput, input, RDG::Access::StorageWrite )
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
             [&]( const RDG::BufferRef input, const uint32_t batch, const RDG::BufferRef outputGrad,
                  const RDG::BufferRef weights )
        {
            return FusedMlpGraphInputs{ .Weights    = weights,
                                        .Input      = input,
                                        .OutputGrad = outputGrad,
                                        .Shape      = kNrcShape,
                                        .Batch      = batch };
        };

        // ---- Inference of this frame's render queries, segment by segment, chunk by chunk, each composited into the
        // sums - with THE INFERENCE WEIGHTS (the optimiser's own before the first step). ----
        const bool           averaged   = m_AdamStep > 0u;
        const RDG::BufferRef inferTable = averaged ? cache.HashEma : cache.HashTable;
        const RDG::BufferRef inferMlp   = averaged ? cache.InferenceWeights : cache.Weights;
        const float          hashScale =
             averaged ? float( 1.0 / ( 1.0 - std::pow( double( kNrcEmaDecay ), double( m_AdamStep ) ) ) ) : 1.0f;
        const uint32_t capacity = trace.QueryCapacity;
        const uint32_t chunk    = std::min( kNrcInferenceChunk, capacity );
        for ( uint32_t segment = 0; segment < trace.PathsPerFrame; ++segment )
            for ( uint32_t first = 0; first < capacity; first += chunk )
            {
                const uint32_t rows    = std::min( chunk, capacity - first );
                const uint32_t counter = uint32_t( NrcCounter::Segments ) + segment;
                const uint32_t base    = segment * capacity;
                const RDG::BufferRef input =
                     AddEncodePass( graph, inferTable, hashScale, trace.Queries, kQueryStride, base + first, rows,
                                    trace.Counters, counter, capacity, base );
                const auto output = m_Mlp->AddInferencePass(
                     graph, mlpInputs( input, Groups( rows ) * kGroup, {}, inferMlp ), m_Path );
                if ( !output )
                    return Common::MakeFormattedError( "NRC inference: {}", output.GetError() );
                NrcPush push;
                push.Rows  = glm::uvec4( base, rows, first, trace.Width );
                push.Extra = glm::uvec4( counter, capacity, 0u, 0u );
                push.Adam  = glm::vec4( composite.PathIntensityClamp, 0.0f, 0.0f, 0.0f );
                graph.AddPass(
                     "NRC: Composite", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_Composite.Layout.Get( m_Composite.Shader ),
                                        Graphic::Renderer::GetPipelineRouteFill( *m_Composite.Pipeline ) )
                              .Storage( NrcSlots::NrcQueriesUB, trace.Queries, RDG::Access::StorageRead )
                              .Storage( NrcSlots::NnOutput, output.GetValue(), RDG::Access::StorageRead )
                              .Storage( NrcSlots::AccumulationTexture, composite.Radiance,
                                        RDG::Access::StorageWrite )
                              .Storage( NrcSlots::DemodulatedDiffuseTexture, composite.DemodulatedDiffuse,
                                        RDG::Access::StorageWrite )
                              .Storage( NrcSlots::DemodulatedSpecularTexture, composite.DemodulatedSpecular,
                                        RDG::Access::StorageWrite )
                              .Storage( NrcSlots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                     },
                     RecordDispatch( m_Composite.Pipeline, push, Groups( rows ), 1 ) );
            }

        // ---- The cache at the self-training suffix ends, before this frame's training. ----
        const RDG::BufferRef selfInput =
             AddEncodePass( graph, cache.HashTable, 1.0f, trace.SelfQueries, kVertexStride, 0, kNrcMaxTrainingPaths,
                            trace.Counters, uint32_t( NrcCounter::SelfQueries ) );
        const auto selfOutput =
             m_Mlp->AddInferencePass( graph, mlpInputs( selfInput, kNrcMaxTrainingPaths, {}, cache.Weights ), m_Path );
        if ( !selfOutput )
            return Common::MakeFormattedError( "NRC self-training inference: {}", selfOutput.GetError() );

        // ---- Training: kNrcTrainingSteps Adam steps of kNrcTrainingBatch rows. ----
        const RDG::BufferRef lossPartials = graph.CreateBuffer(
             RDG::BufferDesc{ uint64_t( kLossPartials ) * sizeof( float ) }, "NRC.LossPartials" );
        for ( uint32_t step = 0; step < kNrcTrainingSteps; ++step )
        {
            const RDG::BufferRef input =
                 AddEncodePass( graph, cache.HashTable, 1.0f, trace.TrainingSamples, kTrainingStride, step * kNrcTrainingBatch,
                                kNrcTrainingBatch, trace.Counters, uint32_t( NrcCounter::TrainingSamples ) );
            const auto forward = m_Mlp->AddForwardPass( graph, mlpInputs( input, kNrcTrainingBatch, {}, cache.Weights ), m_Path );
            if ( !forward )
                return Common::MakeFormattedError( "NRC training forward: {}", forward.GetError() );

            const RDG::BufferRef outputGrad = graph.CreateBuffer(
                 RDG::BufferDesc{ static_cast<uint64_t>( kNrcTrainingBatch ) * kFusedMlpWidth * sizeof( float ) },
                 "NRC.OutputGrad" );
            NrcPush lossPush;
            lossPush.Rows = glm::uvec4( step, kNrcTrainingBatch, kNrcMaxTrainingPaths, 0u );
            graph.AddPass(
                 "NRC: Loss", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Loss.Layout.Get( m_Loss.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_Loss.Pipeline ) )
                          .Storage( NrcSlots::NrcTrainingSamplesUB, trace.TrainingSamples,
                                    RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcSelfQueriesUB, trace.SelfQueries, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcSelfOutputUB, selfOutput.GetValue(), RDG::Access::StorageRead )
                          .Storage( NrcSlots::NnOutput, forward.GetValue().Output, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NnOutputGrad, outputGrad, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcLossPartialsUB, lossPartials, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_Loss.Pipeline, lossPush, Groups( kNrcTrainingBatch ), 1 ) );

            const auto backward = m_Mlp->AddBackwardPasses(
                 graph, mlpInputs( input, kNrcTrainingBatch, outputGrad, cache.Weights ), forward.GetValue(), m_Path );
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
                          .Storage( NrcSlots::NnWeights, cache.Weights, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NnGrads, backward.GetValue().Grads, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcTrainingSamplesUB, trace.TrainingSamples,
                                    RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcHashGradUB, cache.HashGrad, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_InputGrad.Pipeline, gradPush, Groups( kNrcTrainingBatch ), 1 ) );

            const uint32_t t = static_cast<uint32_t>( ++m_AdamStep );
            NrcPush        mlpPush;
            mlpPush.Rows  = glm::uvec4( step, kNrcTrainingBatch, kWeightElements, t );
            mlpPush.Adam  = glm::vec4( kNrcLearningRate, kNrcAdamBeta1, kNrcAdamBeta2, kNrcAdamEpsilon );
            mlpPush.Adam2 = glm::vec4( kNrcWeightDecay, kNrcEmaDecay,
                                       float( 1.0 / ( 1.0 - std::pow( double( kNrcEmaDecay ), double( t ) ) ) ),
                                       0.0f );
            graph.AddPass(
                 "NRC: Adam MLP", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_AdamMlp.Layout.Get( m_AdamMlp.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_AdamMlp.Pipeline ) )
                          .Storage( NrcSlots::NnWeightGrad, backward.GetValue().WeightGrad,
                                    RDG::Access::StorageRead )
                          .Storage( NrcSlots::NrcWeightsUB, cache.Weights, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcAdamMlpUB, cache.AdamMlp, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcWeightsEmaUB, cache.WeightsEma, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcInferenceWeightsUB, cache.InferenceWeights,
                                    RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( NrcPush ) ) );
                 },
                 RecordDispatch( m_AdamMlp.Pipeline, mlpPush, Groups( kWeightElements ), 1 ) );

            NrcPush hashPush = mlpPush;
            hashPush.Rows.z  = kHashElements;
            hashPush.Adam2   = glm::vec4( 0.0f, kNrcEmaDecay, 0.0f, 0.0f );
            // 2D: 1024 groups of 64 per row of the dispatch (the shader's e = x + y * 65536).
            constexpr uint32_t kHashGroupsX = 1024;
            graph.AddPass(
                 "NRC: Adam Hash", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_AdamHash.Layout.Get( m_AdamHash.Shader ),
                                    Graphic::Renderer::GetPipelineRouteFill( *m_AdamHash.Pipeline ) )
                          .Storage( NrcSlots::NrcHashTableUB, cache.HashTable, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcAdamHashUB, cache.AdamHash, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcHashEmaUB, cache.HashEma, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcHashGradUB, cache.HashGrad, RDG::Access::StorageWrite )
                          .Storage( NrcSlots::NrcCountersUB, trace.Counters, RDG::Access::StorageRead )
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
        slot.Paths           = trace.PathsPerFrame;
        graph.Extract( trace.Counters, slot.Counters, RDG::Access::HostRead );
        graph.Extract( lossPartials, slot.LossPartials, RDG::Access::HostRead );
        return BOOLSUCCESS;
    }
} // namespace Desert::Engine::Neural

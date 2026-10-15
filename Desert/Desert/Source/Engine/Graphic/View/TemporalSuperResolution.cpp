// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp:1860-3470,
// adapted: see TemporalSuperResolution.hpp.
#include "TemporalSuperResolution.hpp"

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <span>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8; // every TSR shader: LocalSize(8, 8, 1)

        constexpr const char* kClearPrevTextures   = "TSRClearPrevTextures";
        constexpr const char* kDilateVelocity      = "TSRDilateVelocity";
        constexpr const char* kDecimateHistory     = "TSRDecimateHistory";
        constexpr const char* kRejectShading       = "TSRRejectShading";
        constexpr const char* kSpatialAntiAliasing = "TSRSpatialAntiAliasing";
        constexpr const char* kUpdateHistory       = "TSRUpdateHistory";
        constexpr const char* kResolveHistory      = "TSRResolveHistory";

        constexpr const char* kColorHistory         = "TSR.History.Color";
        constexpr const char* kColorHistoryPrevious = "TSR.History.Color.Previous";
        constexpr const char* kGuideHistory         = "TSR.History.Guide";
        constexpr const char* kGuideHistoryPrevious = "TSR.History.Guide.Previous";
        constexpr const char* kMoireHistory         = "TSR.History.Moire";
        constexpr const char* kMoireHistoryPrevious = "TSR.History.Moire.Previous";
        constexpr std::size_t kColorIndex           = 0;
        constexpr std::size_t kGuideIndex           = 1;
        constexpr std::size_t kMoireIndex           = 2;
        constexpr std::size_t kHistoryCount         = 3;

        // RGBA16F / R32F only: the storage-image formats Vulkan guarantees without
        // shaderStorageImageExtendedFormats, so nothing here depends on a MoltenVK feature bit.
        constexpr auto kColorFormat = Core::Formats::ImageFormat::RGBA16F;
        constexpr auto kDepthFormat = Core::Formats::ImageFormat::R32F;

        // UE TemporalSuperResolution.cpp CVar defaults: r.TSR.History.SampleCount 16 (clamped 8..32 at :1933),
        // r.TSR.ShadingRejection.SampleCount 2, r.TSR.Velocity.WeightClampingSampleCount 4,
        // r.TSR.Velocity.WeightClampingPixelSpeed 1, r.TSR.ShadingRejection.Flickering.Period 2.
        constexpr float kMaxHistorySampleCount             = 16.0f;
        constexpr float kHistoryRejectionSampleCount       = 2.0f;
        constexpr float kVelocityWeightClampingSampleCount = 4.0f;
        constexpr float kVelocityWeightClampingPixelSpeed  = 1.0f;
        constexpr float kFlickeringFramePeriod             = 2.0f;

        uint32_t Groups( uint32_t size )
        {
            return ( size + kGroupSize - 1 ) / kGroupSize;
        }

        using Kind = RDG::ShaderResourceKind;

        std::shared_ptr<const RDG::ShaderBindingLayout> MakeLayout( const char*                  shader,
                                                                    std::vector<RDG::ShaderSlot> slots )
        {
            return std::make_shared<const RDG::ShaderBindingLayout>( RDG::ShaderBindingLayout{
                 .ShaderName = shader, .Slots = std::move( slots ), .PushConstantBytes = 0 } );
        }
    } // namespace

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrClearPrevTexturesLayout()
    {
        static const auto layout =
             MakeLayout( kClearPrevTextures, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                               { "TSRPrevAtomicBuffer", Kind::StorageBuffer } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDilateVelocityLayout()
    {
        static const auto layout =
             MakeLayout( kDilateVelocity, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                            { "u_SceneDepth", Kind::SampledTexture },
                                            { "u_Velocity", Kind::SampledTexture },
                                            { "u_DilatedVelocity", Kind::StorageTexture },
                                            { "u_ClosestDepth", Kind::StorageTexture },
                                            { "TSRPrevAtomicBuffer", Kind::StorageBuffer } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDecimateHistoryLayout()
    {
        static const auto layout =
             MakeLayout( kDecimateHistory, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                             { "u_DilatedVelocity", Kind::SampledTexture },
                                             { "u_ClosestDepth", Kind::SampledTexture },
                                             { "TSRPrevAtomicBuffer", Kind::StorageBuffer },
                                             { "u_PrevGuide", Kind::SampledTexture },
                                             { "u_PrevMoire", Kind::SampledTexture },
                                             { "u_ReprojectedGuide", Kind::StorageTexture },
                                             { "u_ReprojectedMoire", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrRejectShadingLayout()
    {
        static const auto layout = MakeLayout( kRejectShading, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                                 { "u_SceneColor", Kind::SampledTexture },
                                                                 { "u_Exposure", Kind::SampledTexture },
                                                                 { "u_ReprojectedGuide", Kind::SampledTexture },
                                                                 { "u_ReprojectedMoire", Kind::SampledTexture },
                                                                 { "u_HistoryRejection", Kind::StorageTexture },
                                                                 { "u_GuideOut", Kind::StorageTexture },
                                                                 { "u_MoireOut", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrSpatialAntiAliasingLayout()
    {
        static const auto layout =
             MakeLayout( kSpatialAntiAliasing, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                 { "u_SceneColor", Kind::SampledTexture },
                                                 { "u_Exposure", Kind::SampledTexture },
                                                 { "u_HistoryRejection", Kind::SampledTexture },
                                                 { "u_AntiAliasing", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrUpdateHistoryLayout( const bool writesOutput )
    {
        const auto slots = []( const bool withOutput )
        {
            std::vector<RDG::ShaderSlot> list{
                 { "TSRParamsBuffer", Kind::StorageBuffer },    { "u_SceneColor", Kind::SampledTexture },
                 { "u_Exposure", Kind::SampledTexture },        { "u_ResponsiveAA", Kind::SampledTexture },
                 { "u_DilatedVelocity", Kind::SampledTexture }, { "u_HistoryRejection", Kind::SampledTexture },
                 { "u_AntiAliasing", Kind::SampledTexture },    { "u_PrevHistory", Kind::SampledTexture },
                 { "u_HistoryOut", Kind::StorageTexture } };
            if ( withOutput )
                list.push_back( { "u_Output", Kind::StorageTexture } );
            return list;
        };
        static const auto withOutput  = MakeLayout( kUpdateHistory, slots( true ) );
        static const auto historyOnly = MakeLayout( kUpdateHistory, slots( false ) );
        return writesOutput ? withOutput : historyOnly;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrResolveHistoryLayout()
    {
        static const auto layout = MakeLayout( kResolveHistory, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                                  { "u_Exposure", Kind::SampledTexture },
                                                                  { "u_History", Kind::SampledTexture },
                                                                  { "u_Output", Kind::StorageTexture } } );
        return layout;
    }

    TsrQuality ReadTsrQuality( const Common::Scalability::ResolvedQuality& quality )
    {
        using P = Common::Scalability::Parameter;
        // Resolve clamps every row to its spec range, so the values below are inside the ranges the shaders and
        // the extent rule are written for.
        return TsrQuality{ .HistoryScreenPercentage    = quality.As<int>( P::TsrHistoryScreenPercentage ),
                           .HistoryUpdateQuality       = quality.As<int>( P::TsrHistoryUpdateQuality ),
                           .ShadingRejectionFlickering = quality.As<int>( P::TsrShadingRejectionFlickering ) != 0,
                           .RejectionAntiAliasing      = quality.As<int>( P::TsrRejectionAntiAliasing ) };
    }

    RDG::Extent3D TsrHistoryExtent( const ResolutionSplit& split, const TsrQuality& quality )
    {
        const auto side = [&]( uint32_t output )
        {
            const uint64_t scaled =
                 ( static_cast<uint64_t>( output ) *
                        static_cast<uint64_t>( std::max( quality.HistoryScreenPercentage, 1 ) ) +
                   50u ) /
                 100u;
            return static_cast<uint32_t>( std::max<uint64_t>( scaled, 1u ) );
        };
        return RDG::Extent3D{ side( split.Output.Width ), side( split.Output.Height ), 1 };
    }

    TsrParams MakeTsrParams( const ViewFrame& frame, const RDG::Extent3D history, const TsrQuality& quality )
    {
        TsrParams params;
        params.InvViewProjection  = frame.InvViewProjection;
        params.PrevViewProjection = frame.PrevViewProjection;
        params.PrevInvProjection  = glm::inverse( frame.PrevProjection );
        params.RenderSize         = glm::vec2( static_cast<float>( frame.Split.Render.Width ),
                                               static_cast<float>( frame.Split.Render.Height ) );
        params.HistorySize =
             glm::vec2( static_cast<float>( history.Width ), static_cast<float>( history.Height ) );
        params.OutputSize   = glm::vec2( static_cast<float>( frame.Split.Output.Width ),
                                         static_cast<float>( frame.Split.Output.Height ) );
        params.JitterUv     = frame.JitterNdc * glm::vec2( 0.5f, -0.5f ); // Velocity.hpp: uv = ndc * (0.5, -0.5)
        params.HistoryValid = frame.HistoryValid() ? 1.0f : 0.0f;

        // UE :3218-3226. OutputToHistoryResolutionFraction = History / Output.
        const float outputToHistory =
             frame.Split.Output.Width > 0
                  ? static_cast<float>( history.Width ) / static_cast<float>( frame.Split.Output.Width )
                  : 1.0f;
        const float outputToHistorySquare = outputToHistory * outputToHistory;
        params.HistorySampleCount         = kMaxHistorySampleCount / outputToHistorySquare;
        params.HistoryHisteresis          = 1.0f / params.HistorySampleCount;
        params.WeightClampingRejection =
             1.0f - ( kHistoryRejectionSampleCount / outputToHistorySquare ) * params.HistoryHisteresis;
        params.WeightClampingPixelSpeedAmplitude =
             std::clamp( 1.0f - kVelocityWeightClampingSampleCount * params.HistoryHisteresis, 0.0f, 1.0f );
        params.InvWeightClampingPixelSpeed = 1.0f / ( kVelocityWeightClampingPixelSpeed * outputToHistory );
        params.InputToHistoryFactor =
             frame.Split.Render.Width > 0
                  ? static_cast<float>( history.Width ) / static_cast<float>( frame.Split.Render.Width )
                  : 1.0f;
        params.FlickeringFramePeriod      = kFlickeringFramePeriod;
        params.EnableFlickeringHeuristic  = quality.ShadingRejectionFlickering ? 1.0f : 0.0f;
        params.RejectionAntiAliasingSteps = quality.RejectionAntiAliasing >= 2   ? 8.0f
                                            : quality.RejectionAntiAliasing == 1 ? 4.0f
                                                                                 : 0.0f;

        // A perspective projection has w = -z (P[3][3] == 0), an orthographic one w = 1 (P[3][3] == 1).
        const float pixelsHigh       = std::max( params.RenderSize.y, 1.0f );
        const float p11              = std::max( std::abs( frame.PrevProjection[1][1] ), 1e-6f );
        const bool  ortho            = frame.PrevProjection[3][3] == 1.0f;
        params.DepthPixelRadiusScale = ortho ? 0.0f : 1.0f / ( pixelsHigh * p11 );
        params.DepthPixelRadiusBias  = ortho ? 1.0f / ( pixelsHigh * p11 ) : 0.0f;
        return params;
    }

    TemporalSuperResolution::TemporalSuperResolution() = default;

    TemporalSuperResolution::~TemporalSuperResolution() = default;

    bool TemporalSuperResolution::Supports( const ResolutionSplit& split ) const
    {
        return split.Mode == Common::Scalability::ScaleMode::Upscale;
    }

    std::vector<HistoryTextureDesc>
    TemporalSuperResolution::HistoryDescs( const ResolutionSplit&                      split,
                                           const Common::Scalability::ResolvedQuality& quality ) const
    {
        const TsrQuality   tsr = ReadTsrQuality( quality );
        HistoryTextureDesc color;
        color.Desc.Size    = TsrHistoryExtent( split, tsr );
        color.Desc.Format  = kColorFormat;
        color.Name         = kColorHistory;
        color.PreviousName = kColorHistoryPrevious;

        HistoryTextureDesc guide;
        guide.Desc.Size    = RDG::Extent3D{ split.Render.Width, split.Render.Height, 1 };
        guide.Desc.Format  = kColorFormat;
        guide.Name         = kGuideHistory;
        guide.PreviousName = kGuideHistoryPrevious;

        HistoryTextureDesc moire = guide;
        moire.Name               = kMoireHistory;
        moire.PreviousName       = kMoireHistoryPrevious;
        return { color, guide, moire };
    }

    Common::ResultStr<TemporalUpscalerOutputs>
    TemporalSuperResolution::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                        const TemporalUpscalerInputs& inputs ) const
    {
        const auto missing = [&]( std::string_view what ) -> Common::ResultStr<TemporalUpscalerOutputs>
        {
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "TSR: the input {} is not a texture of this graph", what ) );
        };
        if ( !inputs.SceneColor.IsValid() )
            return missing( "SceneColor" );
        if ( !inputs.SceneDepth.IsValid() )
            return missing( "SceneDepth" );
        if ( !inputs.Velocity.IsValid() )
            return missing( "Velocity" );
        if ( !inputs.Exposure.IsValid() )
            return missing( "Exposure" );
        if ( !inputs.ResponsiveAA.IsValid() )
            return missing( "ResponsiveAA" );
        if ( inputs.Quality == nullptr )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", "TSR: TemporalUpscalerInputs::Quality is null (its r.TSR.* rows are read from it)" );
        if ( inputs.History.size() != kHistoryCount )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "TSR: {} histories given, HistoryDescs declares {}", inputs.History.size(),
                                    kHistoryCount ) );
        for ( const HistoryRefs& history : inputs.History )
            if ( !history.Previous.IsValid() || !history.Current.IsValid() )
                return missing( "history (one of TSR.History.Color / Guide / Moire)" );
        const ResolutionSplit& split = frame.Split;
        if ( !Supports( split ) || split.Render.Width == 0 || split.Render.Height == 0 ||
             split.Output.Width == 0 || split.Output.Height == 0 )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "TSR: cannot resolve the split {}x{} -> {}x{} ({} %)", split.Render.Width,
                                    split.Render.Height, split.Output.Width, split.Output.Height,
                                    split.RenderScalePercent ) );

        const TsrQuality    quality = ReadTsrQuality( *inputs.Quality );
        const RDG::Extent3D render{ split.Render.Width, split.Render.Height, 1 };
        const RDG::Extent3D output{ split.Output.Width, split.Output.Height, 1 };
        const RDG::Extent3D history = TsrHistoryExtent( split, quality );
        const HistoryRefs   color   = inputs.History[kColorIndex];
        const HistoryRefs   guide   = inputs.History[kGuideIndex];
        const HistoryRefs   moire   = inputs.History[kMoireIndex];

        const TsrParams      params = MakeTsrParams( frame, history, quality );
        const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ sizeof( TsrParams ) }, "TSR.Params" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( &params, 1 ) ) );
        const RDG::BufferRef prevAtomic = graph.CreateBuffer(
             RDG::BufferDesc{ static_cast<uint64_t>( render.Width ) * render.Height * sizeof( uint32_t ) },
             "TSR.PrevAtomic" );

        const auto texture = [&]( RDG::Extent3D size, Core::Formats::ImageFormat format, const char* name )
        {
            RDG::TextureDesc desc;
            desc.Size   = size;
            desc.Format = format;
            return graph.CreateTexture( desc, name );
        };
        const RDG::TextureRef dilatedVelocity  = texture( render, kColorFormat, "TSR.DilatedVelocity" );
        const RDG::TextureRef closestDepth     = texture( render, kDepthFormat, "TSR.ClosestDepth" );
        const RDG::TextureRef reprojectedGuide = texture( render, kColorFormat, "TSR.ReprojectedGuide" );
        const RDG::TextureRef reprojectedMoire = texture( render, kColorFormat, "TSR.ReprojectedMoire" );
        const RDG::TextureRef historyRejection = texture( render, kColorFormat, "TSR.HistoryRejection" );
        const RDG::TextureRef antiAliasing     = texture( render, kColorFormat, "TSR.AntiAliasing" );
        const RDG::TextureRef sceneColorOutput = texture( output, kColorFormat, "TSR.Output" );
        const bool            historyIsOutput  = history.Width == output.Width && history.Height == output.Height;
        const auto            point            = RDG::SamplerDesc::PointClamp();
        const auto            linear           = RDG::SamplerDesc::LinearClamp();
        const auto            all              = RDG::SubresourceRange::All();
        constexpr auto        sampled          = RDG::Access::SampledCompute;

        // The record-time half of every pass: the pipeline of (shader, defines), dispatched over @p extent.
        const auto dispatch = [this]( const char* shader, std::string defines, RDG::Extent3D extent )
        {
            return [this, shader, defines = std::move( defines ),
                    extent]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                std::string                            error;
                const std::shared_ptr<ComputePipeline> pipeline = PipelineFor( shader, defines, error );
                if ( !pipeline )
                    return Common::MakeError( error );
                const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, Groups( extent.Width ),
                                                                Groups( extent.Height ), 1 );
            };
        };

        graph.AddPass(
             "TSR.ClearPrevTextures", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrClearPrevTexturesLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Storage( "TSRPrevAtomicBuffer", prevAtomic, RDG::Access::StorageWrite );
             },
             dispatch( kClearPrevTextures, "", render ) );

        graph.AddPass(
             "TSR.DilateVelocity", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrDilateVelocityLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Sampled( "u_SceneDepth", inputs.SceneDepth, sampled, all, point )
                      .Sampled( "u_Velocity", inputs.Velocity, sampled, all, point )
                      .Storage( "u_DilatedVelocity", dilatedVelocity, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_ClosestDepth", closestDepth, RDG::Access::StorageWrite, 0 )
                      .Storage( "TSRPrevAtomicBuffer", prevAtomic, RDG::Access::StorageWrite );
             },
             dispatch( kDilateVelocity, "", render ) );

        graph.AddPass(
             "TSR.DecimateHistory", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrDecimateHistoryLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Sampled( "u_DilatedVelocity", dilatedVelocity, sampled, all, point )
                      .Sampled( "u_ClosestDepth", closestDepth, sampled, all, point )
                      .Storage( "TSRPrevAtomicBuffer", prevAtomic, RDG::Access::StorageRead )
                      .Sampled( "u_PrevGuide", guide.Previous, sampled, all, linear )
                      .Sampled( "u_PrevMoire", moire.Previous, sampled, all, linear )
                      .Storage( "u_ReprojectedGuide", reprojectedGuide, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_ReprojectedMoire", reprojectedMoire, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kDecimateHistory, "", render ) );

        graph.AddPass(
             "TSR.RejectShading", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrRejectShadingLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Sampled( "u_SceneColor", inputs.SceneColor, sampled, all, point )
                      .Sampled( "u_Exposure", inputs.Exposure, sampled, all, point )
                      .Sampled( "u_ReprojectedGuide", reprojectedGuide, sampled, all, point )
                      .Sampled( "u_ReprojectedMoire", reprojectedMoire, sampled, all, point )
                      .Storage( "u_HistoryRejection", historyRejection, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_GuideOut", guide.Current, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_MoireOut", moire.Current, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kRejectShading, "", render ) );

        // r.TSR.RejectionAntiAliasingQuality 0: no spatial AA; UpdateHistory reads a zero offset (the same shader
        // writes it at 0 search steps, so UpdateHistory's binding set never changes with the quality).
        graph.AddPass(
             "TSR.SpatialAntiAliasing", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrSpatialAntiAliasingLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Sampled( "u_SceneColor", inputs.SceneColor, sampled, all, point )
                      .Sampled( "u_Exposure", inputs.Exposure, sampled, all, point )
                      .Sampled( "u_HistoryRejection", historyRejection, sampled, all, point )
                      .Storage( "u_AntiAliasing", antiAliasing, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kSpatialAntiAliasing, "", render ) );

        graph.AddPass(
             "TSR.UpdateHistory", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 RDG::BindingBlockBuilder bindings =
                      pass.Bindings( TsrUpdateHistoryLayout( historyIsOutput ), RDG::OtherRouteFill{} );
                 bindings.Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Sampled( "u_SceneColor", inputs.SceneColor, sampled, all, linear )
                      .Sampled( "u_Exposure", inputs.Exposure, sampled, all, point )
                      .Sampled( "u_ResponsiveAA", inputs.ResponsiveAA, sampled, all, point )
                      .Sampled( "u_DilatedVelocity", dilatedVelocity, sampled, all, point )
                      .Sampled( "u_HistoryRejection", historyRejection, sampled, all, point )
                      .Sampled( "u_AntiAliasing", antiAliasing, sampled, all, point )
                      .Sampled( "u_PrevHistory", color.Previous, sampled, all, linear )
                      .Storage( "u_HistoryOut", color.Current, RDG::Access::StorageWrite, 0 );
                 if ( historyIsOutput )
                     bindings.Storage( "u_Output", sceneColorOutput, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kUpdateHistory,
                       std::format( "TSR_UPDATE_QUALITY={};TSR_WRITE_OUTPUT={}", quality.HistoryUpdateQuality,
                                    historyIsOutput ? 1 : 0 ),
                       history ) );

        if ( !historyIsOutput )
            graph.AddPass(
                 "TSR.ResolveHistory", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( TsrResolveHistoryLayout(), RDG::OtherRouteFill{} )
                          .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                          .Sampled( "u_Exposure", inputs.Exposure, sampled, all, point )
                          .Sampled( "u_History", color.Current, sampled, all, linear )
                          .Storage( "u_Output", sceneColorOutput, RDG::Access::StorageWrite, 0 );
                 },
                 dispatch( kResolveHistory, "", output ) );

        return Common::MakeSuccess( TemporalUpscalerOutputs{ .SceneColor = sceneColorOutput } );
    }

    std::shared_ptr<ComputePipeline> TemporalSuperResolution::PipelineFor( const char*        shaderName,
                                                                           const std::string& defines,
                                                                           std::string&       error ) const
    {
        const std::string     key = std::format( "{}|{}", shaderName, defines );
        const std::lock_guard lock( m_PipelineMutex );
        if ( const auto found = m_Pipelines.find( key ); found != m_Pipelines.end() )
            return found->second;

        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        if ( !service )
        {
            error = std::format( "TSR: no shader service to compile '{}' ({})", shaderName, defines );
            return nullptr;
        }
        // "" is the default program (AcquireVariant refuses the empty variant); otherwise one define per ';'.
        std::shared_ptr<Shader> shader;
        if ( defines.empty() )
            shader = service->GetByName( shaderName );
        else
        {
            Core::ShaderVariant variant;
            std::size_t         begin = 0;
            while ( begin <= defines.size() )
            {
                const std::size_t end = std::min( defines.find( ';', begin ), defines.size() );
                variant.Defines.emplace_back( defines.substr( begin, end - begin ) );
                begin = end + 1;
            }
            shader = service->AcquireVariant( shaderName, variant );
        }
        if ( !shader )
        {
            error = std::format( "TSR: the shader '{}' ({}) did not compile — expected "
                                 "Editor/Resources/Shaders/Programs/TemporalSuperResolution/{}.shader",
                                 shaderName, defines, shaderName );
            return nullptr;
        }
        const std::string pipelineName = std::format( "{} ({})", shaderName, defines );
        const auto        built = ComputePipeline::Create( { .Shader = shader, .DebugName = pipelineName } );
        if ( !built )
        {
            error = std::format( "TSR: pipeline '{}' ({}): {}", shaderName, defines, built.GetError() );
            return nullptr;
        }
        m_Pipelines.emplace( key, built.GetValue() );
        return built.GetValue();
    }
} // namespace Desert::Graphic

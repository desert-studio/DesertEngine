// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp:1860-3470,
// adapted: see TemporalSuperResolution.hpp.
#include "TemporalSuperResolution.hpp"

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Project/RendererSettings.hpp>
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
        constexpr const char* kRecordPersistent    = "TSRRecordPersistentFrame";
        constexpr const char* kDetectThinGeometry  = "TSRDetectThinGeometry";
        constexpr const char* kMeasureFlickeringLuma = "TSRMeasureFlickeringLuma";
        // UE r.TSR.ThinGeometryDetection.Coverage.ShadingRange 3 (VaryingRange) and .MinKeepLineContrast 0.30,
        // their defaults; UE's other ranges (Foliage, FoliageAndHair, AllExceptUnlit) are its console's choices.
        constexpr bool  kThinGeometryShadingRangeIsVarying = true;
        constexpr float kThinGeometryMinKeepLineContrast   = 0.30f;
        // TSR.DetectThinGeometry's tile (Programs/TemporalSuperResolution/TSRDetectThinGeometry.shader: 16x16 minus
        // UE's overscan of 3 on each side): the pixels one group writes per side.
        constexpr uint32_t kThinGeometryTileOutput = 10;
        // TSR.RejectShading's output tile per side (TSRRejectShading.shader kTile): 8 lanes write 8x8, or 6x6 with
        // thin geometry, whose lerp-clamped history box costs one more apron from the same 16x16 load.
        constexpr uint32_t kRejectShadingTile             = kGroupSize;
        constexpr uint32_t kRejectShadingThinGeometryTile = 6;

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
        // r.TSR.Resurrection: kTsrPersistentFrameCount persistent colour frames, then as many guide frames.
        constexpr std::size_t kPersistentColorIndex = kHistoryCount;
        constexpr std::size_t kPersistentGuideIndex = kHistoryCount + kTsrPersistentFrameCount;
        constexpr std::size_t kHistoryCountWithResurrection = kHistoryCount + 2 * kTsrPersistentFrameCount;
        // The project's r.TSR.ThinGeometryDetection: the coverage history, after every other.
        constexpr const char* kCoverageHistory         = "TSR.History.Coverage";
        constexpr const char* kCoverageHistoryPrevious = "TSR.History.Coverage.Previous";
        [[nodiscard]] std::size_t DeclaredHistoryCount( const TsrQuality& quality )
        {
            return ( quality.Resurrection ? kHistoryCountWithResurrection : kHistoryCount ) +
                   ( quality.ThinGeometryDetection ? 1 : 0 );
        }
        constexpr std::array<const char*, kTsrPersistentFrameCount> kPersistentColorNames{
             "TSR.History.Color.Persistent0", "TSR.History.Color.Persistent1" };
        constexpr std::array<const char*, kTsrPersistentFrameCount> kPersistentGuideNames{
             "TSR.History.Guide.Persistent0", "TSR.History.Guide.Persistent1" };
        // UE r.TSR.ReprojectionField.AntiAliasPixelSpeed (0.125), a project constant in UE.
        constexpr float kReprojectionFieldAntiAliasPixelSpeed = 0.125f;

        // RGBA16F / R32F only: the storage-image formats Vulkan guarantees without
        // shaderStorageImageExtendedFormats, so nothing here depends on a MoltenVK feature bit.
        constexpr auto kColorFormat = Core::Formats::ImageFormat::RGBA16F;
        constexpr auto kDepthFormat = Core::Formats::ImageFormat::R32F;
        constexpr auto kUintFormat  = Core::Formats::ImageFormat::R32_UINT;

        // UE TemporalSuperResolution.cpp CVar defaults: r.TSR.History.SampleCount 16 (clamped 8..32 at :1933),
        // r.TSR.ShadingRejection.SampleCount 2, r.TSR.Velocity.WeightClampingSampleCount 4,
        // r.TSR.Velocity.WeightClampingPixelSpeed 1, r.TSR.ShadingRejection.Flickering.Period 2.
        constexpr float kMaxHistorySampleCount             = 16.0f;
        constexpr float kHistoryRejectionSampleCount       = 2.0f;
        constexpr float kVelocityWeightClampingSampleCount = 4.0f;
        constexpr float kVelocityWeightClampingPixelSpeed  = 1.0f;
        constexpr float kFlickeringFramePeriod             = 2.0f;

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
                                            { "u_SceneDepth", Kind::Texture },
                                            { "u_Velocity", Kind::Texture },
                                            { "u_PointClampSampler", Kind::Sampler },
                                            { "u_DilatedVelocity", Kind::StorageTexture },
                                            { "u_ClosestDepth", Kind::StorageTexture },
                                            { "TSRPrevAtomicBuffer", Kind::StorageBuffer },
                                            { "u_ReprojectionJacobian", Kind::StorageTexture },
                                            { "u_ReprojectionBoundary", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDetectThinGeometryLayout( const bool intensityLines )
    {
        const auto slots = []( const bool withLuma )
        {
            std::vector<RDG::ShaderSlot> list{ { "TSRParamsBuffer", Kind::StorageBuffer },
                                               { "u_SceneDepth", Kind::Texture },
                                               { "u_ThinGeometryCoverage", Kind::Texture },
                                               { "u_ReprojectedCoverage", Kind::Texture },
                                               { "u_ReprojectionBoundary", Kind::Texture },
                                               { "u_PointClampSampler", Kind::Sampler },
                                               { "u_ThinGeometry", Kind::StorageTexture },
                                               { "u_CoverageOut", Kind::StorageTexture } };
            if ( withLuma )
                list.push_back( { "u_FlickeringLuma", Kind::Texture } );
            return list;
        };
        static const auto withLuma = MakeLayout( kDetectThinGeometry, slots( true ) );
        static const auto plain    = MakeLayout( kDetectThinGeometry, slots( false ) );
        return intensityLines ? withLuma : plain;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrMeasureFlickeringLumaLayout()
    {
        static const auto layout =
             MakeLayout( kMeasureFlickeringLuma, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                   { "u_SceneColor", Kind::Texture },
                                                   { "u_Exposure", Kind::Texture },
                                                   { "u_PointClampSampler", Kind::Sampler },
                                                   { "u_FlickeringLuma", Kind::StorageTexture } } );
        return layout;
    }

    bool TsrThinGeometryVaryingRange( const TsrQuality& quality )
    {
        return quality.ThinGeometryDetection && kThinGeometryShadingRangeIsVarying &&
               ( quality.ShadingRejectionFlickering || kThinGeometryMinKeepLineContrast != 0.0f );
    }

    bool TsrThinGeometryIntensityLines( const TsrQuality& quality )
    {
        return TsrThinGeometryVaryingRange( quality ) && kThinGeometryMinKeepLineContrast != 0.0f;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDecimateHistoryLayout( const bool thinGeometry )
    {
        const auto slots = []( const bool withCoverage )
        {
            std::vector<RDG::ShaderSlot> list{ { "TSRParamsBuffer", Kind::StorageBuffer },
                                             { "u_DilatedVelocity", Kind::Texture },
                                             { "u_ClosestDepth", Kind::Texture },
                                             { "TSRPrevAtomicBuffer", Kind::StorageBuffer },
                                             { "u_PrevGuide", Kind::Texture },
                                             { "u_PrevMoire", Kind::Texture },
                                             { "u_ReprojectedGuide", Kind::StorageTexture },
                                             { "u_ReprojectedMoire", Kind::StorageTexture },
                                             { "u_ResurrectionGuide", Kind::Texture },
                                             { "u_Exposure", Kind::Texture },
                                             { "u_LinearClampSampler", Kind::Sampler },
                                             { "u_PointClampSampler", Kind::Sampler },
                                             { "u_ResurrectedGuide", Kind::StorageTexture } };
            if ( withCoverage )
            {
                list.push_back( { "u_PrevCoverage", Kind::Texture } );
                list.push_back( { "u_ReprojectedCoverage", Kind::StorageTexture } );
            }
            return list;
        };
        static const auto withCoverage = MakeLayout( kDecimateHistory, slots( true ) );
        static const auto plain        = MakeLayout( kDecimateHistory, slots( false ) );
        return thinGeometry ? withCoverage : plain;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrRejectShadingLayout( const bool thinGeometry )
    {
        const auto slots = []( const bool withThinGeometry )
        {
            std::vector<RDG::ShaderSlot> list{ { "TSRParamsBuffer", Kind::StorageBuffer },
                                             { "u_SceneColor", Kind::Texture },
                                             { "u_Exposure", Kind::Texture },
                                             { "u_ReprojectedGuide", Kind::Texture },
                                             { "u_ReprojectedMoire", Kind::Texture },
                                             { "u_ResurrectedGuide", Kind::Texture },
                                             { "u_PointClampSampler", Kind::Sampler },
                                             { "u_HistoryRejection", Kind::StorageTexture },
                                             { "u_GuideOut", Kind::StorageTexture },
                                             { "u_MoireOut", Kind::StorageTexture },
                                             { "u_AntiAliasInput", Kind::StorageTexture } };
            if ( withThinGeometry )
                list.push_back( { "u_ThinGeometry", Kind::Texture } );
            return list;
        };
        static const auto withThinGeometry = MakeLayout( kRejectShading, slots( true ) );
        static const auto plain            = MakeLayout( kRejectShading, slots( false ) );
        return thinGeometry ? withThinGeometry : plain;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrSpatialAntiAliasingLayout()
    {
        static const auto layout =
             MakeLayout( kSpatialAntiAliasing, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                 { "u_AntiAliasInput", Kind::Texture },
                                                 { "u_LinearClampSampler", Kind::Sampler },
                                                 { "u_AntiAliasing", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrUpdateHistoryLayout( const bool writesOutput )
    {
        const auto slots = []( const bool withOutput )
        {
            std::vector<RDG::ShaderSlot> list{ { "TSRParamsBuffer", Kind::StorageBuffer },
                                               { "u_SceneColor", Kind::Texture },
                                               { "u_Exposure", Kind::Texture },
                                               { "u_ResponsiveAA", Kind::Texture },
                                               { "u_DilatedVelocity", Kind::Texture },
                                               { "u_HistoryRejection", Kind::Texture },
                                               { "u_AntiAliasing", Kind::Texture },
                                               { "u_PrevHistory", Kind::Texture },
                                               { "u_ClosestDepth", Kind::Texture },
                                               { "u_ReprojectionJacobian", Kind::Texture },
                                               { "u_ReprojectionBoundary", Kind::Texture },
                                               { "u_ResurrectionHistory", Kind::Texture },
                                               { "u_LinearClampSampler", Kind::Sampler },
                                               { "u_PointClampSampler", Kind::Sampler },
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
                                                                  { "u_Exposure", Kind::Texture },
                                                                  { "u_History", Kind::Texture },
                                                                  { "u_LinearClampSampler", Kind::Sampler },
                                                                  { "u_PointClampSampler", Kind::Sampler },
                                                                  { "u_Output", Kind::StorageTexture } } );
        return layout;
    }

    const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrRecordPersistentFrameLayout()
    {
        static const auto layout = MakeLayout( kRecordPersistent, { { "TSRParamsBuffer", Kind::StorageBuffer },
                                                                    { "u_Exposure", Kind::Texture },
                                                                    { "u_Source", Kind::Texture },
                                                                    { "u_PointClampSampler", Kind::Sampler },
                                                                    { "u_Destination", Kind::StorageTexture } } );
        return layout;
    }

    int TsrSpatialAntiAliasingIterations( const TsrQuality& quality )
    {
        return quality.RejectionAntiAliasing >= 2 ? 8 : quality.RejectionAntiAliasing == 1 ? 3 : 0;
    }

    int TsrResurrectionSlot( const TsrHistory& previous, const uint64_t thisFrame )
    {
        // The oldest recorded frame: NextSlot when it is recorded (both are), else the other one.
        int slot = -1;
        if ( previous.Frames[previous.NextSlot].Recorded )
            slot = static_cast<int>( previous.NextSlot );
        else
        {
            for ( uint32_t i = 0; i < kTsrPersistentFrameCount; ++i )
                if ( previous.Frames[i].Recorded )
                {
                    slot = static_cast<int>( i );
                    break;
                }
        }
        // UE bCanResurrectHistory: resurrecting the previous frame is the ordinary reprojection.
        if ( slot >= 0 && previous.Frames[static_cast<std::size_t>( slot )].Frame + 1 == thisFrame )
            return -1;
        return slot;
    }

    TsrQuality ReadTsrQuality( const Common::Scalability::ResolvedQuality& quality )
    {
        using P = Common::Scalability::Parameter;
        // Resolve clamps every row to its spec range, so the values below are inside the ranges the shaders and
        // the extent rule are written for.
        return TsrQuality{ .HistoryScreenPercentage    = quality.As<int>( P::TsrHistoryScreenPercentage ),
                           .HistoryUpdateQuality       = quality.As<int>( P::TsrHistoryUpdateQuality ),
                           .ShadingRejectionFlickering = quality.As<int>( P::TsrShadingRejectionFlickering ) != 0,
                           .RejectionAntiAliasing      = quality.As<int>( P::TsrRejectionAntiAliasing ),
                           .ReprojectionField          = quality.As<int>( P::TsrReprojectionField ) != 0,
                           .Resurrection               = quality.As<int>( P::TsrResurrection ) != 0,
                           .ThinGeometryDetection = Project::CurrentRendererSettings().TsrThinGeometryDetection };
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

    TsrParams MakeTsrParams( const ViewFrame& frame, const RDG::Extent3D history, const TsrQuality& quality,
                             const TsrPersistentFrame* resurrection )
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
        params.FlickeringFramePeriod     = kFlickeringFramePeriod;
        params.EnableFlickeringHeuristic = quality.ShadingRejectionFlickering ? 1.0f : 0.0f;
        params.ReprojectionField         = quality.ReprojectionField ? 1.0f : 0.0f;
        params.InvProjection             = frame.InvProjection;
        params.Projection                = frame.Projection;
        // UE :2531, OutputToInputResolutionFraction = Render / Output.
        const float outputToInput =
             frame.Split.Output.Width > 0
                  ? static_cast<float>( frame.Split.Render.Width ) / static_cast<float>( frame.Split.Output.Width )
                  : 1.0f;
        const float antiAliasPixelSpeed =
             std::max( kReprojectionFieldAntiAliasPixelSpeed / std::max( outputToInput, 1e-6f ), 1.0f / 64.0f );
        params.ReprojectionFieldAntiAliasVelocityThreshold = antiAliasPixelSpeed * antiAliasPixelSpeed;
        if ( resurrection != nullptr )
        {
            params.ClipToResurrectionClip = resurrection->ViewProjection * frame.InvViewProjection;
            params.ResurrectionEnabled    = 1.0f;
        }

        // A perspective projection has w = -z (P[3][3] == 0), an orthographic one w = 1 (P[3][3] == 1).
        const float pixelsHigh       = std::max( params.RenderSize.y, 1.0f );
        const float p11              = std::max( std::abs( frame.PrevProjection[1][1] ), 1e-6f );
        const bool  ortho            = frame.PrevProjection[3][3] == 1.0f;
        params.DepthPixelRadiusScale = ortho ? 0.0f : 1.0f / ( pixelsHigh * p11 );
        params.DepthPixelRadiusBias  = ortho ? 1.0f / ( pixelsHigh * p11 ) : 0.0f;
        params.ThinGeometryDetection = quality.ThinGeometryDetection ? 1.0f : 0.0f;
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

        std::vector<HistoryTextureDesc> descs{ color, guide, moire };
        // r.TSR.Resurrection: the persistent frames (UE: the persistent slices of the history arrays).
        for ( std::size_t i = 0; tsr.Resurrection && i < kTsrPersistentFrameCount; ++i )
        {
            HistoryTextureDesc persistent = color;
            persistent.Lifetime           = HistoryLifetime::Persistent;
            persistent.Name               = kPersistentColorNames[i];
            persistent.PreviousName       = "";
            descs.push_back( persistent );
        }
        for ( std::size_t i = 0; tsr.Resurrection && i < kTsrPersistentFrameCount; ++i )
        {
            HistoryTextureDesc persistent = guide;
            persistent.Lifetime           = HistoryLifetime::Persistent;
            persistent.Name               = kPersistentGuideNames[i];
            persistent.PreviousName       = "";
            descs.push_back( persistent );
        }
        // The project's r.TSR.ThinGeometryDetection: the coverage history (UE History.CoverageArray, R8 there; R32F
        // here, a storage format Vulkan guarantees).
        if ( tsr.ThinGeometryDetection )
        {
            HistoryTextureDesc coverage = guide;
            coverage.Desc.Format        = kDepthFormat;
            coverage.Name               = kCoverageHistory;
            coverage.PreviousName       = kCoverageHistoryPrevious;
            descs.push_back( coverage );
        }
        return descs;
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
        if ( !inputs.Black.IsValid() )
            return missing( "Black" );
        if ( inputs.Quality == nullptr )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", "TSR: TemporalUpscalerInputs::Quality is null (its r.TSR.* rows are read from it)" );
        const TsrQuality  quality         = ReadTsrQuality( *inputs.Quality );
        const bool        intensityLines  = TsrThinGeometryIntensityLines( quality );
        if ( intensityLines && !inputs.OpaqueLuma.IsValid() )
            return missing( "OpaqueLuma (AddOpaquePasses' TSR.FlickeringLuma)" );
        const std::size_t declaredHistory = DeclaredHistoryCount( quality );
        if ( quality.ThinGeometryDetection && !inputs.ThinGeometryCoverage.IsValid() )
            return missing( "ThinGeometryCoverage" );
        if ( inputs.History.size() != declaredHistory )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "TSR: {} histories given, HistoryDescs declares {}", inputs.History.size(),
                                    declaredHistory ) );
        for ( const HistoryRefs& history : inputs.History )
            if ( !history.Previous.IsValid() || !history.Current.IsValid() )
                return missing( "history (one of TSR.History.Color / Guide / Moire / *.Persistent* / Coverage)" );
        const ResolutionSplit& split = frame.Split;
        if ( !Supports( split ) || split.Render.Width == 0 || split.Render.Height == 0 ||
             split.Output.Width == 0 || split.Output.Height == 0 )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "{}", std::format( "TSR: cannot resolve the split {}x{} -> {}x{} ({} %)", split.Render.Width,
                                    split.Render.Height, split.Output.Width, split.Output.Height,
                                    split.RenderScalePercent ) );

        const RDG::Extent3D render{ split.Render.Width, split.Render.Height, 1 };
        const RDG::Extent3D output{ split.Output.Width, split.Output.Height, 1 };
        const RDG::Extent3D history = TsrHistoryExtent( split, quality );
        const HistoryRefs   color   = inputs.History[kColorIndex];
        const HistoryRefs   guide   = inputs.History[kGuideIndex];
        const HistoryRefs   moire   = inputs.History[kMoireIndex];

        // The persistent-frame records (UE FTSRHistory). A reset history (camera cut, resize, method change)
        // starts over: the persistent textures' contents belong to the frames before it.
        auto        records  = std::make_shared<TsrHistory>();
        const auto* previous = dynamic_cast<const TsrHistory*>( inputs.PrevHistory.get() );
        if ( previous != nullptr && frame.HistoryValid() )
        {
            *records = *previous;
            ++records->AccumulatedFrames;
        }
        const int resurrectionSlot =
             quality.Resurrection && previous != nullptr && frame.HistoryValid()
                  ? TsrResurrectionSlot( *previous, records->AccumulatedFrames )
                  : -1;
        const TsrPersistentFrame* resurrection =
             resurrectionSlot >= 0 ? &previous->Frames[static_cast<std::size_t>( resurrectionSlot )] : nullptr;
        // UE: a persistent rolling index every PersistentFrameInterval frames, the first frame included.
        const bool recordThisFrame =
             quality.Resurrection && records->AccumulatedFrames % kTsrPersistentFrameInterval == 0;
        const uint32_t recordSlot = records->NextSlot;

        TsrParams params              = MakeTsrParams( frame, history, quality, resurrection );
        params.ThinGeometryFrameIndex = static_cast<float>( records->AccumulatedFrames % 64u );
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
        const RDG::TextureRef antiAliasInput   = texture( render, kColorFormat, "TSR.AntiAliasInput" );
        const RDG::TextureRef antiAliasing     = texture( render, kUintFormat, "TSR.AntiAliasing" );
        const RDG::TextureRef reprojJacobian   = texture( render, kUintFormat, "TSR.ReprojectionJacobian" );
        const RDG::TextureRef reprojBoundary   = texture( render, kUintFormat, "TSR.ReprojectionBoundary" );
        const RDG::TextureRef resurrectedGuide = texture( render, kColorFormat, "TSR.ResurrectedGuide" );
        // The project's r.TSR.ThinGeometryDetection: the coverage history pair, the reprojected coverage and the
        // thin-geometry weight (UE R8Output[ThinGeometryTextureIndex]); invalid refs without it (no pass names them).
        const HistoryRefs coverage =
             quality.ThinGeometryDetection ? inputs.History[declaredHistory - 1] : HistoryRefs{};
        const RDG::TextureRef reprojectedCoverage = quality.ThinGeometryDetection
                                                         ? texture( render, kDepthFormat, "TSR.ReprojectedCoverage" )
                                                         : RDG::TextureRef{};
        const RDG::TextureRef thinGeometry =
             quality.ThinGeometryDetection ? texture( render, kUintFormat, "TSR.ThinGeometry" ) : RDG::TextureRef{};
        // The resurrection frame's textures; with none this frame the shaders read nothing from them
        // (u_ResurrectionEnabled 0) and the slot holds System.Black (UE: BlackArrayDummy).
        const RDG::TextureRef resurrectionGuide =
             resurrection != nullptr
                  ? inputs.History[kPersistentGuideIndex + static_cast<std::size_t>( resurrectionSlot )].Previous
                  : inputs.Black;
        const RDG::TextureRef resurrectionColor =
             resurrection != nullptr
                  ? inputs.History[kPersistentColorIndex + static_cast<std::size_t>( resurrectionSlot )].Previous
                  : inputs.Black;
        const RDG::TextureRef sceneColorOutput = texture( output, kColorFormat, "TSR.Output" );
        const bool            historyIsOutput  = history.Width == output.Width && history.Height == output.Height;
        const auto            point            = RDG::SamplerDesc::PointClamp();
        const auto            linear           = RDG::SamplerDesc::LinearClamp();
        const auto            all              = RDG::SubresourceRange::All();
        constexpr auto        sampled          = RDG::Access::SampledCompute;

        // Every pipeline this frame's chain dispatches, made now: one that does not compile, or that the device
        // refuses (VulkanShader::Reflect: shared memory above maxComputeSharedMemorySize), refuses TSR for the frame
        // by name before a pass is added, instead of failing one pass at record time.
        const std::string decimateDefines =
             std::format( "TSR_THIN_GEOMETRY={}", quality.ThinGeometryDetection ? 1 : 0 );
        // UE DIM_THIN_GEOMETRY_DETECTION: RejectShading reads TSR.ThinGeometry and relaxes its clamp box by it.
        const std::string rejectDefines = decimateDefines;
        // UE DIM_VARYING_RANGE_DETECTION / DIM_INTENSITY_LINE_DETECTION (TsrThinGeometryVaryingRange).
        const std::string thinGeometryDefines =
             std::format( "TSR_THIN_VARYING_RANGE={};TSR_THIN_INTENSITY_LINE={}",
                          TsrThinGeometryVaryingRange( quality ) ? 1 : 0, intensityLines ? 1 : 0 );
        const std::string spatialDefines =
             std::format( "TSR_SPATIAL_AA_ITERATIONS={}", TsrSpatialAntiAliasingIterations( quality ) );
        const std::string updateDefines = std::format( "TSR_UPDATE_QUALITY={};TSR_WRITE_OUTPUT={}",
                                                       quality.HistoryUpdateQuality, historyIsOutput ? 1 : 0 );
        {
            std::vector<std::pair<const char*, std::string>> used{
                 { kClearPrevTextures, "" },
                 { kDilateVelocity, "" },
                 { kDecimateHistory, decimateDefines },
                 { kRejectShading, rejectDefines },
                 { kSpatialAntiAliasing, spatialDefines },
                 { kUpdateHistory, updateDefines } };
            if ( quality.ThinGeometryDetection )
                used.emplace_back( kDetectThinGeometry, thinGeometryDefines );
            if ( !historyIsOutput )
                used.emplace_back( kResolveHistory, "" );
            if ( recordThisFrame )
            {
                used.emplace_back( kRecordPersistent, "TSR_RECORD_GUIDE=0" );
                used.emplace_back( kRecordPersistent, "TSR_RECORD_GUIDE=1" );
            }
            for ( const auto& [shader, defines] : used )
            {
                std::string error;
                if ( !PipelineFor( shader, defines, error ) )
                    return Common::MakeFormattedError<TemporalUpscalerOutputs>( "{}",
                                                                                std::format( "TSR refused: {}", error ) );
            }
        }

        // The record-time half of every pass: the pipeline of (shader, defines), dispatched over @p extent.
        const auto dispatch = [this]( const char* shader, std::string defines, RDG::Extent3D extent,
                                      uint32_t tile = kGroupSize )
        {
            return [this, shader, defines = std::move( defines ), extent,
                    tile]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                std::string                            error;
                const std::shared_ptr<ComputePipeline> pipeline = PipelineFor( shader, defines, error );
                if ( !pipeline )
                    return Common::MakeError( error );
                const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                return Renderer::GetInstance().DispatchCompute( bindings, *pipeline,
                                                                ( extent.Width + tile - 1 ) / tile,
                                                                ( extent.Height + tile - 1 ) / tile, 1 );
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
                      .Texture( "u_SceneDepth", inputs.SceneDepth, sampled, all )
                      .Texture( "u_Velocity", inputs.Velocity, sampled, all )
                      .Sampler( "u_PointClampSampler", point )
                      .Storage( "u_DilatedVelocity", dilatedVelocity, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_ClosestDepth", closestDepth, RDG::Access::StorageWrite, 0 )
                      .Storage( "TSRPrevAtomicBuffer", prevAtomic, RDG::Access::StorageWrite )
                      .Storage( "u_ReprojectionJacobian", reprojJacobian, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_ReprojectionBoundary", reprojBoundary, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kDilateVelocity, "", render ) );

        graph.AddPass(
             "TSR.DecimateHistory", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 RDG::BindingBlockBuilder bindings =
                      pass.Bindings( TsrDecimateHistoryLayout( quality.ThinGeometryDetection ), RDG::OtherRouteFill{} );
                 bindings.Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Texture( "u_DilatedVelocity", dilatedVelocity, sampled, all )
                      .Texture( "u_ClosestDepth", closestDepth, sampled, all )
                      .Storage( "TSRPrevAtomicBuffer", prevAtomic, RDG::Access::StorageRead )
                      .Texture( "u_PrevGuide", guide.Previous, sampled, all )
                      .Texture( "u_PrevMoire", moire.Previous, sampled, all )
                      .Storage( "u_ReprojectedGuide", reprojectedGuide, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_ReprojectedMoire", reprojectedMoire, RDG::Access::StorageWrite, 0 )
                      .Texture( "u_ResurrectionGuide", resurrectionGuide, sampled, all )
                      .Sampler( "u_LinearClampSampler", linear )
                      .Sampler( "u_PointClampSampler", point )
                      .Texture( "u_Exposure", inputs.Exposure, sampled, all )
                      .Storage( "u_ResurrectedGuide", resurrectedGuide, RDG::Access::StorageWrite, 0 );
                 if ( quality.ThinGeometryDetection )
                     bindings.Texture( "u_PrevCoverage", coverage.Previous, sampled, all )
                          .Storage( "u_ReprojectedCoverage", reprojectedCoverage, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kDecimateHistory, decimateDefines, render ) );

        // The project's r.TSR.ThinGeometryDetection (UE: after DecimateHistory, before RejectShading).
        if ( quality.ThinGeometryDetection )
            graph.AddPass(
                 "TSR.DetectThinGeometry", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     RDG::BindingBlockBuilder bindings =
                          pass.Bindings( TsrDetectThinGeometryLayout( intensityLines ), RDG::OtherRouteFill{} );
                     bindings.Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                          .Texture( "u_SceneDepth", inputs.SceneDepth, sampled, all )
                          .Texture( "u_ThinGeometryCoverage", inputs.ThinGeometryCoverage, sampled, all )
                          .Sampler( "u_PointClampSampler", point )
                          .Texture( "u_ReprojectedCoverage", reprojectedCoverage, sampled, all )
                          .Texture( "u_ReprojectionBoundary", reprojBoundary, sampled, all )
                          .Storage( "u_ThinGeometry", thinGeometry, RDG::Access::StorageWrite, 0 )
                          .Storage( "u_CoverageOut", coverage.Current, RDG::Access::StorageWrite, 0 );
                     if ( intensityLines )
                         bindings.Texture( "u_FlickeringLuma", inputs.OpaqueLuma, sampled, all );
                 },
                 dispatch( kDetectThinGeometry, thinGeometryDefines, render, kThinGeometryTileOutput ) );

        graph.AddPass(
             "TSR.RejectShading", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 RDG::BindingBlockBuilder bindings =
                      pass.Bindings( TsrRejectShadingLayout( quality.ThinGeometryDetection ), RDG::OtherRouteFill{} );
                 bindings.Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Texture( "u_SceneColor", inputs.SceneColor, sampled, all )
                      .Texture( "u_Exposure", inputs.Exposure, sampled, all )
                      .Texture( "u_ReprojectedGuide", reprojectedGuide, sampled, all )
                      .Texture( "u_ReprojectedMoire", reprojectedMoire, sampled, all )
                      .Texture( "u_ResurrectedGuide", resurrectedGuide, sampled, all )
                      .Sampler( "u_PointClampSampler", point )
                      .Storage( "u_HistoryRejection", historyRejection, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_GuideOut", guide.Current, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_MoireOut", moire.Current, RDG::Access::StorageWrite, 0 )
                      .Storage( "u_AntiAliasInput", antiAliasInput, RDG::Access::StorageWrite, 0 );
                 if ( quality.ThinGeometryDetection )
                     bindings.Texture( "u_ThinGeometry", thinGeometry, sampled, all );
             },
             dispatch( kRejectShading, rejectDefines, render,
                       quality.ThinGeometryDetection ? kRejectShadingThinGeometryTile : kRejectShadingTile ) );

        // r.TSR.RejectionAntiAliasingQuality 0: no spatial AA; UpdateHistory reads a zero offset (the same shader
        // writes it at 0 iterations, so UpdateHistory's binding set never changes with the quality).
        graph.AddPass(
             "TSR.SpatialAntiAliasing", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrSpatialAntiAliasingLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Texture( "u_AntiAliasInput", antiAliasInput, sampled, all )
                      .Sampler( "u_LinearClampSampler", linear )
                      .Storage( "u_AntiAliasing", antiAliasing, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kSpatialAntiAliasing, spatialDefines, render ) );

        graph.AddPass(
             "TSR.UpdateHistory", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 RDG::BindingBlockBuilder bindings =
                      pass.Bindings( TsrUpdateHistoryLayout( historyIsOutput ), RDG::OtherRouteFill{} );
                 bindings.Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Texture( "u_SceneColor", inputs.SceneColor, sampled, all )
                      .Texture( "u_Exposure", inputs.Exposure, sampled, all )
                      .Texture( "u_ResponsiveAA", inputs.ResponsiveAA, sampled, all )
                      .Texture( "u_DilatedVelocity", dilatedVelocity, sampled, all )
                      .Texture( "u_HistoryRejection", historyRejection, sampled, all )
                      .Texture( "u_AntiAliasing", antiAliasing, sampled, all )
                      .Texture( "u_PrevHistory", color.Previous, sampled, all )
                      .Texture( "u_ClosestDepth", closestDepth, sampled, all )
                      .Texture( "u_ReprojectionJacobian", reprojJacobian, sampled, all )
                      .Texture( "u_ReprojectionBoundary", reprojBoundary, sampled, all )
                      .Texture( "u_ResurrectionHistory", resurrectionColor, sampled, all )
                      .Sampler( "u_LinearClampSampler", linear )
                      .Sampler( "u_PointClampSampler", point )
                      .Storage( "u_HistoryOut", color.Current, RDG::Access::StorageWrite, 0 );
                 if ( historyIsOutput )
                     bindings.Storage( "u_Output", sceneColorOutput, RDG::Access::StorageWrite, 0 );
             },
             dispatch( kUpdateHistory, updateDefines, history ) );

        if ( !historyIsOutput )
            graph.AddPass(
                 "TSR.ResolveHistory", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( TsrResolveHistoryLayout(), RDG::OtherRouteFill{} )
                          .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                          .Texture( "u_Exposure", inputs.Exposure, sampled, all )
                          .Texture( "u_History", color.Current, sampled, all )
                          .Sampler( "u_LinearClampSampler", linear )
                          .Sampler( "u_PointClampSampler", point )
                          .Storage( "u_Output", sceneColorOutput, RDG::Access::StorageWrite, 0 );
                 },
                 dispatch( kResolveHistory, "", output ) );

        // r.TSR.Resurrection: this frame becomes the oldest persistent frame (UE: a persistent rolling index).
        if ( recordThisFrame )
        {
            const auto record = [&]( const char* name, RDG::TextureRef source, RDG::TextureRef destination,
                                     int guideVariant, RDG::Extent3D extent )
            {
                graph.AddPass(
                     name, RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( TsrRecordPersistentFrameLayout(), RDG::OtherRouteFill{} )
                              .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                              .Texture( "u_Exposure", inputs.Exposure, sampled, all )
                              .Texture( "u_Source", source, sampled, all )
                              .Sampler( "u_PointClampSampler", point )
                              .Storage( "u_Destination", destination, RDG::Access::StorageWrite, 0 );
                     },
                     dispatch( kRecordPersistent, std::format( "TSR_RECORD_GUIDE={}", guideVariant ), extent ) );
            };
            record( "TSR.RecordPersistentFrame.Color", color.Current,
                    inputs.History[kPersistentColorIndex + recordSlot].Current, 0, history );
            record( "TSR.RecordPersistentFrame.Guide", guide.Current,
                    inputs.History[kPersistentGuideIndex + recordSlot].Current, 1, render );
            records->Frames[recordSlot] = TsrPersistentFrame{ .Recorded       = true,
                                                              .Frame          = records->AccumulatedFrames,
                                                              .ViewProjection = frame.ViewProjection };
            records->NextSlot           = ( recordSlot + 1 ) % kTsrPersistentFrameCount;
        }

        return Common::MakeSuccess(
             TemporalUpscalerOutputs{ .SceneColor = sceneColorOutput, .NewHistory = std::move( records ) } );
    }

    Common::ResultStr<RDG::TextureRef>
    TemporalSuperResolution::AddOpaquePasses( RDG::Builder& graph, const ViewFrame& frame,
                                              const TemporalUpscalerOpaqueInputs& inputs ) const
    {
        if ( inputs.Quality == nullptr )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", "TSR: TemporalUpscalerOpaqueInputs::Quality is null (its r.TSR.* rows are read from it)" );
        const TsrQuality quality = ReadTsrQuality( *inputs.Quality );
        if ( !TsrThinGeometryIntensityLines( quality ) )
            return Common::MakeSuccess( RDG::TextureRef{} );
        if ( !inputs.SceneColor.IsValid() || !inputs.Exposure.IsValid() )
            return Common::MakeFormattedError<RDG::TextureRef>(
                 "{}", std::format( "TSR.MeasureFlickeringLuma: the input {} is not a texture of this graph",
                                    inputs.SceneColor.IsValid() ? "Exposure" : "SceneColor" ) );
        std::string error;
        if ( !PipelineFor( kMeasureFlickeringLuma, "", error ) )
            return Common::MakeFormattedError<RDG::TextureRef>( "{}", std::format( "TSR refused: {}", error ) );

        const RDG::Extent3D render{ frame.Split.Render.Width, frame.Split.Render.Height, 1 };
        // The shared TSR parameter block (TSRCommon.glslh), of which this pass reads the render extent.
        const TsrParams params =
             MakeTsrParams( frame, TsrHistoryExtent( frame.Split, quality ), quality, nullptr );
        const RDG::BufferRef buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( TsrParams ) }, "TSR.MeasureFlickeringLuma.Params" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span( &params, 1 ) ) );
        RDG::TextureDesc desc;
        desc.Size                  = render;
        desc.Format                = kDepthFormat; // one float channel
        const RDG::TextureRef luma = graph.CreateTexture( desc, "TSR.FlickeringLuma" );

        graph.AddPass(
             "TSR.MeasureFlickeringLuma", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( TsrMeasureFlickeringLumaLayout(), RDG::OtherRouteFill{} )
                      .Storage( "TSRParamsBuffer", buffer, RDG::Access::StorageRead )
                      .Texture( "u_SceneColor", inputs.SceneColor, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Texture( "u_Exposure", inputs.Exposure, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                      .Storage( "u_FlickeringLuma", luma, RDG::Access::StorageWrite, 0 );
             },
             [this, render]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 std::string                            pipelineError;
                 const std::shared_ptr<ComputePipeline> pipeline =
                      PipelineFor( kMeasureFlickeringLuma, "", pipelineError );
                 if ( !pipeline )
                     return Common::MakeError( pipelineError );
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute(
                      bindings, *pipeline, ( render.Width + kGroupSize - 1 ) / kGroupSize,
                      ( render.Height + kGroupSize - 1 ) / kGroupSize, 1 );
             } );
        return Common::MakeSuccess( luma );
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

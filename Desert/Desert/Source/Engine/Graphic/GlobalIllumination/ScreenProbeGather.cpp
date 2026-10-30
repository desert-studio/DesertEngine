// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenScreenProbeGather.cpp (the pass sequence:
// uniform placement, adaptive placement per level, trace, filter + irradiance, integrate, temporal) and UE 5.8
// LumenScreenProbeFiltering.cpp FilterScreenProbes (the spatial filter's passes and its disocclusion input),
// adapted as described in ScreenProbeGather.hpp.
#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/DistanceField/DistanceFieldShared.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ScreenProbeGatherShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ScreenProbeGatherPlaceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeSpeedUB );
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ScreenProbeGatherTraceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridWorldCellsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferEmissiveTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheRadianceAtlasTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeTracedTexture );
        DESERT_SHADER_PARAMETER( Texture, HzbTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneColorHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, DistanceFieldBrickAtlasTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFAssetData );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFIndirection );
        DESERT_SHADER_PARAMETER( StorageBuffer, DFObjects );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIMeshSDFObjectRowsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIMeshSDFCulledUB );
        DESERT_SHADER_PARAMETER( StorageTexture, ShortRangeAoOutTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeTraceHitUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeSpeedUB );
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeCompactArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeCompactAllocatorUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeCompactTexelsUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ScreenProbeGatherFilterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeTracedTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeFilteredTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeSHUB );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeRadiance );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeBackfaceUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeTraceHitUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeMovingUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ScreenProbeGatherIntegrateShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeSHUB );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeFilteredTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeDiffuseHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeSpecularHistoryTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeDiffuseOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeSpecularOutTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeBackfaceUB );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeBackfaceOutTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeMovingUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, ShortRangeAoTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
    };
    struct ScreenProbeGatherDisocclusionShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWord );
        DESERT_SHADER_PARAMETER( Texture, DiffuseIndirectHistory );
        DESERT_SHADER_PARAMETER( Texture, HistoryGuide );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeDisocclusionUB );
    };
    struct ScreenProbeGatherSpatialFilterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIndexUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeDisocclusionUB );
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeRadiance );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredScreenProbeRadiance );
    };
    struct ScreenProbeGatherTemporalShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWord );
        DESERT_SHADER_PARAMETER( Texture, DiffuseIndirect );
        DESERT_SHADER_PARAMETER( Texture, RoughSpecularIndirect );
        DESERT_SHADER_PARAMETER( Texture, BackfaceDiffuseIndirect );
        DESERT_SHADER_PARAMETER( Texture, DiffuseIndirectHistory );
        DESERT_SHADER_PARAMETER( Texture, RoughSpecularIndirectHistory );
        DESERT_SHADER_PARAMETER( Texture, BackfaceDiffuseIndirectHistory );
        DESERT_SHADER_PARAMETER( Texture, HistoryGuide );
        DESERT_SHADER_PARAMETER( StorageTexture, NewHistoryDiffuseIndirect );
        DESERT_SHADER_PARAMETER( StorageTexture, NewHistoryRoughSpecularIndirect );
        DESERT_SHADER_PARAMETER( StorageTexture, NewHistoryBackfaceDiffuseIndirect );
        DESERT_SHADER_PARAMETER( StorageTexture, NewHistoryGuide );
    };

} // namespace

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        using Common::Math::DivideRoundUp;

        constexpr std::array<std::string_view, 11> kScreenProbeShaderNames{
             "ScreenProbePlace",  "ScreenProbeShortRangeAO", "ScreenProbeTraceScreen",  "ScreenProbeCompactTraces",
             "ScreenProbeTrace",  "ScreenProbeTrace",        "ScreenProbeDisocclusion", "ScreenProbeSpatialFilter",
             "ScreenProbeFilter", "ScreenProbeIntegrate",    "ScreenProbeTemporal" };

        // UE r.Lumen.ScreenProbeGather.SpatialFilterNumPasses.
        constexpr uint32_t kSpatialFilterPasses = 3;

        // The defines of the world legs' two permutations of ScreenProbeTrace (besides TracerVariant's).
        constexpr std::string_view kTraceNearFieldDefine = "SP_TRACE_NEAR_FIELD";
        constexpr std::string_view kTraceFarFieldDefine  = "SP_TRACE_FAR_FIELD";

        // ScreenProbeCompactArgsUB: x = groups, y = z = 1 (a dispatch's three counts; w unused); the allocator: 0.
        constexpr std::array<uint32_t, 4> kCompactArgsReset{ 0u, 1u, 1u, 0u };
        constexpr uint32_t                kCompactAllocatorReset = 0u;

        // The near field's end along a ray (ScreenProbeCompactTraces' MaxDistance for the near-field list): the
        // software tracer's mesh SDFs trace up to DF_MESH_SDF_TRACE_DISTANCE (GITraceNearField); a ray the screen
        // took past it has nothing left there.
        [[nodiscard]] float NearFieldEnd( const Tracer tracer )
        {
            return tracer == Tracer::SoftwareDistanceField ? DistanceField::DFShared::DF_MESH_SDF_TRACE_DISTANCE
                                                           : std::numeric_limits<float>::max();
        }

        struct CompactPush
        {
            float    MaxDistance;
            uint32_t FarField;
        };

        constexpr std::array<std::string_view, ScreenProbeLayout::kMaxLevels> kPlaceNames{
             "ScreenProbes: Place Uniform", "ScreenProbes: Place Adaptive 1", "ScreenProbes: Place Adaptive 2",
             "ScreenProbes: Place Adaptive 3", "ScreenProbes: Place Adaptive 4" };

        std::shared_ptr<Image2D>
        CreateTarget( const std::string_view tag, const uint32_t width, const uint32_t height,
                      const Core::Formats::ImageFormat format = Core::Formats::ImageFormat::RGBA16F )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( tag ),
                 .Width      = width,
                 .Height     = height,
                 .Format     = format,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        }
    } // namespace

    ScreenProbeGather::ScreenProbeGather()  = default;
    ScreenProbeGather::~ScreenProbeGather() = default;

    namespace
    {
        // The layout permutation every screen-probe program is compiled under (Common/ScreenProbes.glslh
        // SP_DOWNSAMPLE_LOG2 / SP_TRACED_TEXELS): its groupshared arrays and per-probe workgroups are SP_RES² wide.
        std::vector<std::string> ScreenProbeLayoutDefines( const uint32_t downsample, const uint32_t tracedTexels )
        {
            return { std::format( "SP_DOWNSAMPLE_LOG2={}", std::countr_zero( downsample ) ),
                     std::format( "SP_TRACED_TEXELS={}", tracedTexels ) };
        }

        // A layout-only program's permutation; cooked (ProgramVariants).
        Core::ShaderVariant LayoutVariant( const uint32_t downsample, const uint32_t tracedTexels )
        {
            return Core::ShaderVariant{ .Defines       = ScreenProbeLayoutDefines( downsample, tracedTexels ),
                                        .ShippedByCook = true };
        }

        // Trace's permutation: TracerVariant's defines, the leg, then the layout's; cooked (ProgramVariants).
        Core::ShaderVariant TraceVariant( const Pipeline& pipeline, const std::string_view leg,
                                          const uint32_t downsample, const uint32_t tracedTexels )
        {
            Core::ShaderVariant variant = TracerVariant( pipeline );
            variant.Defines.emplace_back( leg );
            for ( std::string& define : ScreenProbeLayoutDefines( downsample, tracedTexels ) )
                variant.Defines.push_back( std::move( define ) );
            variant.ShippedByCook = true;
            return variant;
        }
    } // namespace

    Common::BoolResultStr ScreenProbeGather::Initialize()
    {
        const ScreenProbeLayout defaults;
        return EnsureLayoutPrograms( defaults.Downsample, defaults.TracedTexels );
    }

    Common::BoolResultStr ScreenProbeGather::EnsureLayoutPrograms( const uint32_t downsample,
                                                                   const uint32_t tracedTexels )
    {
        if ( downsample == m_ProgramDownsample && tracedTexels == m_ProgramTracedTexels )
            return BOOLSUCCESS;
        if ( !ScreenProbeLayoutSupported( downsample, tracedTexels ) )
            return RefuseLayout( downsample, tracedTexels, {}, false,
                                 std::format( "screen probes: the layout {} px / {} texels is not one the shaders "
                                              "compile (8, 16, 32 or 64 px a probe; 4..16 traced texels a side)",
                                              downsample, tracedTexels ) );
        const auto                shaders = Runtime::ResourceRegistry::GetShaderService();
        const Core::ShaderVariant variant = LayoutVariant( downsample, tracedTexels );
        for ( uint32_t program = 0; program < ProgramCount; ++program )
        {
            if ( program == TraceNearField || program == TraceFarField )
                continue; // tracer permutations, EnsureTraceProgram
            const std::string name( kScreenProbeShaderNames[program] );
            auto              acquired = shaders->AcquireVariant( name, variant );
            if ( !acquired )
                return RefuseLayout( downsample, tracedTexels, name, true,
                                     std::format( "screen probes: shader {} ({} px, {} texels) was refused: {}",
                                                  name, downsample, tracedTexels, acquired.GetError() ) );
            auto shader   = acquired.ExtractValue();
            auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !pipeline )
                return RefuseLayout( downsample, tracedTexels, name, false,
                                     std::format( "screen probes: {}: {}", name, pipeline.GetError() ) );
            m_Shaders[program]   = std::move( shader );
            m_Pipelines[program] = pipeline.GetValue();
        }
        m_ProgramDownsample   = downsample;
        m_ProgramTracedTexels = tracedTexels;
        m_LayoutRefusal.reset(); // a later refusal is a new one, and is reported
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScreenProbeGather::RefuseLayout( const uint32_t downsample, const uint32_t tracedTexels,
                                                           std::string program, const bool service,
                                                           std::string error )
    {
        // The same refusal asked again keeps its Reported; a different layout or program is a new one.
        if ( !m_LayoutRefusal || m_LayoutRefusal->Downsample != downsample ||
             m_LayoutRefusal->TracedTexels != tracedTexels || m_LayoutRefusal->Program != program ||
             m_LayoutRefusal->Service != service )
            m_LayoutRefusal = LayoutRefusal{ .Downsample   = downsample,
                                             .TracedTexels = tracedTexels,
                                             .Program      = std::move( program ),
                                             .Service      = service };
        return Common::MakeError<bool>( std::move( error ) );
    }

    bool ScreenProbeGather::ClaimLayoutRefusalReport()
    {
        // Asked with no standing refusal it answers true, as ShaderService::ClaimRefusalReport does: whoever asks
        // holds a refusal, and suppressing one nobody recorded would be a silent failure.
        if ( !m_LayoutRefusal )
            return true;
        if ( m_LayoutRefusal->Service )
            return Runtime::ResourceRegistry::GetShaderService()->ClaimRefusalReport(
                 m_LayoutRefusal->Program,
                 LayoutVariant( m_LayoutRefusal->Downsample, m_LayoutRefusal->TracedTexels ) );
        return !std::exchange( m_LayoutRefusal->Reported, true );
    }

    Common::BoolResultStr ScreenProbeGather::EnsureTraceProgram( const Pipeline& pipeline )
    {
        if ( m_Pipelines[TraceNearField] && m_Pipelines[TraceFarField] && pipeline == m_TraceProgramFor &&
             m_TraceProgramDownsample == m_ProgramDownsample && m_TraceProgramTracedTexels == m_ProgramTracedTexels )
            return BOOLSUCCESS;
        for ( const auto& [program, leg] : { std::pair{ TraceNearField, kTraceNearFieldDefine },
                                             std::pair{ TraceFarField, kTraceFarFieldDefine } } )
        {
            const std::string   name( kScreenProbeShaderNames[program] );
            const Core::ShaderVariant variant = TraceVariant( pipeline, leg, m_ProgramDownsample, m_ProgramTracedTexels );
            auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, variant );
            if ( !acquired )
                return Common::MakeFormattedError<bool>( "screen probes: shader {} ({}, {}, {}) was refused: {}",
                                                         name, TracerDefine( pipeline.Tracer ),
                                                         HitLightingDefine( pipeline.Hits ), leg,
                                                         acquired.GetError() );
            auto shader  = acquired.ExtractValue();
            auto compute = ComputePipeline::Create(
                 { .Shader    = shader,
                   .DebugName = std::format( "{} ({}, {})", name, TracerDefine( pipeline.Tracer ), leg ) } );
            if ( !compute )
                return Common::MakeFormattedError<bool>( "screen probes: {} ({}): {}", name, leg,
                                                         compute.GetError() );
            m_Shaders[program]   = std::move( shader );
            m_Pipelines[program] = compute.GetValue();
        }
        m_TraceProgramFor          = pipeline;
        m_TraceProgramDownsample   = m_ProgramDownsample;
        m_TraceProgramTracedTexels = m_ProgramTracedTexels;
        return BOOLSUCCESS;
    }

    std::vector<Core::CookedProgramVariant> ScreenProbeGather::ProgramVariants( const uint32_t downsample,
                                                                              const uint32_t tracedTexels )
    {
        std::vector<Core::CookedProgramVariant> variants;
        for ( uint32_t program = 0; program < ProgramCount; ++program )
            if ( program != TraceNearField && program != TraceFarField )
                variants.push_back( { std::string( kScreenProbeShaderNames[program] ),
                                      LayoutVariant( downsample, tracedTexels ) } );
        for ( const Pipeline& tracer : kTracerPermutations )
            for ( const auto& [program, leg] : { std::pair{ TraceNearField, kTraceNearFieldDefine },
                                                 std::pair{ TraceFarField, kTraceFarFieldDefine } } )
                variants.push_back( { std::string( kScreenProbeShaderNames[program] ),
                                      TraceVariant( tracer, leg, downsample, tracedTexels ) } );
        return variants;
    }

    Common::BoolResultStr ScreenProbeGather::EnsureImages( const glm::uvec2         extent,
                                                           const ScreenProbeLayout& layout )
    {
        if ( m_Traced && extent == m_Extent && layout.AtlasRows == m_AtlasRows &&
             layout.AtlasProbesRow == m_AtlasProbesRow && layout.TracedTexels == m_ImageTracedTexels )
            return BOOLSUCCESS;
        const uint32_t row = layout.AtlasProbesRow;
        const uint32_t res = layout.TracedTexels;
        m_Traced               = CreateTarget( "ScreenProbes.Traced", row * res, layout.AtlasRows * res );
        m_Filtered = CreateTarget( "ScreenProbes.Filtered", row * ( res + 2u ), layout.AtlasRows * ( res + 2u ) );
        m_ShortRangeAO         = CreateTarget( "ScreenProbes.ShortRangeAO", extent.x, extent.y );
        if ( !m_Traced || !m_Filtered || !m_ShortRangeAO )
            return Common::MakeError<bool>( "screen probes: an image could not be created" );
        m_Extent    = extent;
        m_AtlasRows         = layout.AtlasRows;
        m_AtlasProbesRow    = layout.AtlasProbesRow;
        m_ImageTracedTexels = layout.TracedTexels;
        return BOOLSUCCESS;
    }

    Common::ResultStr<ScreenProbeGatherFrame>
    ScreenProbeGather::AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                                  const ScreenProbeGatherInputs& inputs, ScreenProbeGatherHistory& history )
    {
        if ( inputs.Extent.x == 0 || inputs.Extent.y == 0 || inputs.Extent.x >= 4096 || inputs.Extent.y >= 4096 )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>(
                 "screen probes: the G-buffer extent {}x{} is outside 1..4095 (a probe packs its pixel in 24 "
                 "bits)",
                 inputs.Extent.x, inputs.Extent.y );
        if ( !inputs.GIFrame.IsValid() || !inputs.RadianceCache.Radiance.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the GI frame record or the radiance cache of this frame is missing" );
        if ( const std::string_view missing = TracerFrameMissing( inputs.Tracer ); !missing.empty() )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>( "screen probes: {}", missing );
        // A hit reads the cards first (GIHitRadiance) unless the hardware tracer lights every hit in place.
        const bool readsCards = inputs.Tracer.Pipeline.Hits == HitLighting::SurfaceCache;
        if ( const std::string_view missing = SurfaceCacheFrameMissing( inputs.SurfaceCache );
             readsCards && !missing.empty() )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>( "screen probes: {}", missing );
        if ( const auto programs = EnsureLayoutPrograms( inputs.Downsample, inputs.TracedTexels ); !programs )
            return Common::MakeError<ScreenProbeGatherFrame>( programs.GetError() );
        if ( const auto traced = EnsureTraceProgram( inputs.Tracer.Pipeline ); !traced )
            return Common::MakeError<ScreenProbeGatherFrame>( traced.GetError() );
        if ( !inputs.HZB.IsValid() || !inputs.SceneColorHistory.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the screen trace's HZB or scene colour history is missing (bind the system black "
                 "texture when the view has none)" );
        if ( !inputs.Velocity.IsValid() || !inputs.GBufferShadingWord.IsValid() || !inputs.Black.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the view's velocity, the G-buffer's shading word or the system black texture is "
                 "missing (the temporal filter reprojects by the velocity, tells foliage by the shading word and "
                 "binds black for a history the view does not hold)" );
        const uint32_t          maxImageDimension = EngineContext::GetInstance().GetCapabilities().MaxTexture2DSize;
        const ScreenProbeLayout layout            = MakeScreenProbeLayout(
             inputs.Extent, inputs.AdaptiveProbePercent, inputs.Downsample, inputs.TracedTexels, maxImageDimension );
        if ( !ScreenProbeLayoutFits( layout ) )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>(
                 "screen probes: {} uniform probes ({}x{} at {} px) with {} traced texels a side do not fit a {} px "
                 "atlas (the device's maxImageDimension2D); raise GlobalIllumination.ScreenProbeDownsample or lower "
                 "ScreenProbeTracedTexels",
                 layout.UniformProbes, layout.Grid.x, layout.Grid.y, layout.Downsample, layout.TracedTexels,
                 maxImageDimension );
        if ( const auto images = EnsureImages( inputs.Extent, layout ); !images )
            return Common::MakeError<ScreenProbeGatherFrame>( images.GetError() );

        const RDG::TextureRef traced       = import( m_Traced, "ScreenProbes.Traced" );
        const RDG::TextureRef filtered     = import( m_Filtered, "ScreenProbes.Filtered" );
        const RDG::TextureRef shortRangeAO = import( m_ShortRangeAO, "ScreenProbes.ShortRangeAO" );
        // The integrate's output: this frame's lighting, which the temporal filter blends with the view's history
        // (or the gather's result as it is when the denoiser is skipped).
        using History = ScreenProbeGatherHistory;
        const ViewExtent extent{ .Width = inputs.Extent.x, .Height = inputs.Extent.y };
        const auto       integrated = [&]( const History::Texture texture, const char* name )
        { return graph.CreateTexture( History::Desc( texture, extent ), name ); };
        const RDG::TextureRef diffuse  = integrated( History::DiffuseIndirect, "ScreenProbes.Diffuse" );
        const RDG::TextureRef specular = integrated( History::RoughSpecularIndirect, "ScreenProbes.Specular" );
        const RDG::TextureRef backface = integrated( History::BackfaceDiffuseIndirect, "ScreenProbes.Backface" );

        // The view's history set (UE FScreenProbeGatherTemporalState), read only when the view's previous frame is
        // a previous frame of this one: not after a cut, a teleport or a resize (ViewFrame::HistoryValid).
        History::Refs previous{};
        if ( inputs.Denoise )
            previous = history.RegisterPrevious( graph, extent );
        const bool historyReadable =
             inputs.Denoise && inputs.ViewHistoryValid &&
             std::ranges::all_of( previous, []( const RDG::TextureRef& texture ) { return texture.IsValid(); } );
        const auto historyOrBlack = [&]( const History::Texture texture )
        { return historyReadable ? previous[texture] : inputs.Black; };

        ScreenProbeParamsPayload params;
        // The temporal filter's previous world position of a pixel (its NDC.xyz minus the velocity).
        params.PrevInvViewProjection = glm::inverse( inputs.PrevViewProjection );

        params.PrevViewProjection = inputs.PrevViewProjection;
        // w = 1 / PrevPreExposure: ScreenProbeTraceScreen undoes the scene colour history's pre-exposure.
        params.PrevCamera =
             glm::vec4( inputs.PrevCameraPosition, 1.0f / std::max( inputs.PrevPreExposure, 1e-30f ) );
        params.Jitter             = glm::vec4( inputs.JitterNdc, inputs.PrevJitterNdc );
        params.Grid               = glm::uvec4( layout.Grid, layout.UniformProbes, layout.AdaptiveProbes );
        const glm::uvec2 jitter   = ScreenProbeJitter( ++m_FrameIndex, layout.Downsample );
        const uint32_t   flags    = ( historyReadable ? kScreenProbeHistoryValid : 0u ) |
                               ( inputs.ShortRangeAO ? kScreenProbeShortRangeAO : 0u ) |
                               ( inputs.SceneColorHistoryReadable ? kScreenProbeScreenTrace : 0u );
        params.Atlas =
             glm::uvec4( layout.AtlasProbesRow, m_FrameIndex, flags, jitter.x | ( jitter.y << 16u ) );

        const RDG::BufferRef paramsBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( params ) }, "ScreenProbes.Params" );
        graph.QueueBufferUpload( paramsBuffer, std::as_bytes( std::span( &params, 1 ) ) );
        const RDG::BufferRef index =
             graph.CreateBuffer( RDG::BufferDesc{ layout.IndexEntries * 4ull }, "ScreenProbes.Index" );
        const RDG::BufferRef probes = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( ScreenProbePayload ) ) },
             "ScreenProbes.Probes" );
        const RDG::BufferRef sh = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * ScreenProbeLayout::kSHCoefficients * 16ull }, "ScreenProbes.SH" );
        const RDG::BufferRef backfaceIrradiance =
             graph.CreateBuffer( RDG::BufferDesc{ layout.Probes() * 16ull }, "ScreenProbes.BackfaceIrradiance" );
        // The fast update's chain (UE ScreenProbeWorldSpeed -> the trace hit's moving bit -> ScreenProbeMoving ->
        // LightIsMoving): each probe's world speed (the placement), and the share of its rays that hit something
        // moving relative to it (the filter, from the trace hit buffer).
        const RDG::BufferRef speed = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) }, "ScreenProbes.Speed" );
        const RDG::BufferRef moving =
             graph.CreateBuffer( RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) },
                                 "ScreenProbes.Moving" );

        constexpr RDG::Access kW   = RDG::Access::StorageWrite;
        constexpr RDG::Access kR   = RDG::Access::StorageRead;
        constexpr RDG::Access kS   = RDG::Access::SampledCompute;
        const auto            all  = RDG::SubresourceRange::All();
        const auto            gbuf = [&]( RDG::BindingBlockBuilder& block )
        {
            block.Texture( ScreenProbeGatherShaderParameters::GBufferDepthTexture, inputs.GBufferDepth, kS, all )
                 .Sampler( ScreenProbeGatherShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( ScreenProbeGatherShaderParameters::GBufferBTexture, inputs.GBufferB, kS, all );
        };

        // Place: level 0 (uniform, and the tiles' adaptive counts reset), then the two adaptive levels, each
        // reading the probes the coarser ones placed.
        for ( uint32_t level = 0; level < layout.Levels; ++level )
        {
            ComputePipeline* const pipeline = m_Pipelines[Place].get();
            const uint32_t         spacing  = layout.Downsample >> level;
            const glm::uvec2       groups( DivideRoundUp( DivideRoundUp( inputs.Extent.x, spacing ), 8u ),
                                           DivideRoundUp( DivideRoundUp( inputs.Extent.y, spacing ), 8u ) );
            graph.AddPass(
                 kPlaceNames[level], RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Place].Get( m_Shaders[Place] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherPlaceShaderParameters::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( ScreenProbeGatherPlaceShaderParameters::ScreenProbeIndexUB, index, kW )
                          .Storage( ScreenProbeGatherPlaceShaderParameters::ScreenProbesUB, probes, kW )
                          .Storage( ScreenProbeGatherPlaceShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherPlaceShaderParameters::ScreenProbeSpeedUB, speed, kW )
                          .Texture( ScreenProbeGatherPlaceShaderParameters::Velocity, inputs.Velocity, kS, all )
                          .Sampler( ScreenProbeGatherPlaceShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( uint32_t ) ) );
                     gbuf( block );
                 },
                 [pipeline, level, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &level, static_cast<uint32_t>( sizeof( level ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        // Short-range AO: the bent normal and AO of every pixel, from the scene depth (UE
        // ScreenSpaceShortRangeAOCS). Off by scalability, the integrate reads the shading normal unoccluded (its
        // flag) and the composite applies SSAO to the gather instead.
        if ( inputs.ShortRangeAO )
        {
            ComputePipeline* const pipeline = m_Pipelines[ShortRangeAO].get();
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
            graph.AddPass(
                 "ScreenProbes: ShortRangeAO", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[ShortRangeAO].Get( m_Shaders[ShortRangeAO] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ShortRangeAoOutTexture, shortRangeAO,
                                    kW );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        // TRACE (UE LumenScreenProbeTracing.cpp TraceScreenProbes): the screen; the rays it left open compacted;
        // the tracer's near field over them; every ray still without a hit compacted; the far field, the radiance
        // cache and the sky over those. Each ray's state between the legs: the trace hit buffer
        // (Common/ScreenProbeTracing.glslh).
        const uint32_t       probeGroups = layout.Probes();
        const uint32_t       kTexels     = layout.TracedTexels * layout.TracedTexels;
        const uint64_t       rayBytes    = static_cast<uint64_t>( probeGroups ) * kTexels * sizeof( uint32_t );
        const RDG::BufferRef traceHit = graph.CreateBuffer( RDG::BufferDesc{ rayBytes }, "ScreenProbes.TraceHit" );
        {
            ComputePipeline* const pipeline = m_Pipelines[TraceScreen].get();
            graph.AddPass(
                 "ScreenProbes: Trace Screen", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[TraceScreen].Get( m_Shaders[TraceScreen] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::RadianceCacheParamsUB,
                                    inputs.RadianceCache.Params, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTraceHitUB, traceHit, kW )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTracedTexture, traced, kW )
                          .Texture( ScreenProbeGatherTraceShaderParameters::HzbTexture, inputs.HZB, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTraceShaderParameters::SceneColorHistoryTexture,
                                    inputs.SceneColorHistory, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeSpeedUB, speed, kR )
                          .Texture( ScreenProbeGatherTraceShaderParameters::Velocity, inputs.Velocity, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() );
                 },
                 [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }

        // One compaction: the open rays' list and its dispatch arguments (ScreenProbeCompactTraces.shader).
        struct Compacted
        {
            RDG::BufferRef Args;      // the world leg's dispatch (IndirectArgs)
            RDG::BufferRef Allocator; // the rays listed
            RDG::BufferRef Texels;
        };
        const auto compact = [&]( const std::string_view name, const CompactPush push ) -> Compacted
        {
            const Compacted list{ graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactArgsReset ) },
                                                      std::format( "ScreenProbes.CompactArgs ({})", name ) ),
                                  graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactAllocatorReset ) },
                                                      std::format( "ScreenProbes.CompactAllocator ({})", name ) ),
                                  graph.CreateBuffer( RDG::BufferDesc{ rayBytes },
                                                      std::format( "ScreenProbes.CompactTexels ({})", name ) ) };
            graph.QueueBufferUpload( list.Args, std::as_bytes( std::span( kCompactArgsReset ) ) );
            graph.QueueBufferUpload( list.Allocator, std::as_bytes( std::span( &kCompactAllocatorReset, 1 ) ) );
            ComputePipeline* const pipeline = m_Pipelines[CompactTraces].get();
            graph.AddPass(
                 std::format( "ScreenProbes: Compact Traces ({})", name ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[CompactTraces].Get( m_Shaders[CompactTraces] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTraceHitUB, traceHit, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactArgsUB, list.Args,
                                    kW )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactAllocatorUB,
                                    list.Allocator, kW )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactTexelsUB,
                                    list.Texels, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( CompactPush ) ) );
                 },
                 [pipeline, probeGroups, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
            return list;
        };

        // One world leg over a compacted list, one thread per ray (ScreenProbeTrace.shader's permutation).
        const auto traceWorld = [&]( const Program program, const std::string_view name, const Compacted list )
        {
            ComputePipeline* const pipeline = m_Pipelines[program].get();
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[program].Get( m_Shaders[program] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     builder.Read( list.Args, RDG::Access::IndirectArgs );
                     block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::PointLightsUB, inputs.PointLights, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::SpotLightsUB, inputs.SpotLights, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::LightGridParamsUB,
                                    inputs.WorldGridParams, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::LightGridWorldCellsUB,
                                    inputs.WorldGridCells, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::RadianceCacheParamsUB,
                                    inputs.RadianceCache.Params, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::RadianceCacheIndirectionUB,
                                    inputs.RadianceCache.Indirection, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::RadianceCacheProbesUB,
                                    inputs.RadianceCache.Probes, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTraceHitUB, traceHit, kW )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactAllocatorUB,
                                    list.Allocator, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactTexelsUB,
                                    list.Texels, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeSpeedUB, speed, kR );
                     DeclareTracerReads( block, inputs.Tracer );
                     if ( readsCards )
                         DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
                     block.Texture( ScreenProbeGatherTraceShaderParameters::EnvironmentSpecularTexture,
                                    inputs.EnvSpecular, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( ScreenProbeGatherTraceShaderParameters::GBufferDepthTexture,
                                    inputs.GBufferDepth, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTraceShaderParameters::GBufferATexture, inputs.GBufferA, kS,
                                    all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::GBufferEmissiveTexture,
                                    inputs.GBufferEmissive, kS, all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheIrradianceTexture,
                                    inputs.RadianceCache.Irradiance, kS, all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheDepthTexture,
                                    inputs.RadianceCache.Depth, kS, all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheRadianceAtlasTexture,
                                    inputs.RadianceCache.Radiance, kS, all )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTracedTexture, traced, kW );
                 },
                 [pipeline, args = list.Args]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, 0u );
                 } );
        };

        traceWorld( TraceNearField, "ScreenProbes: Trace Near Field",
                    compact( "near field", CompactPush{ NearFieldEnd( inputs.Tracer.Pipeline.Tracer ), 0u } ) );
        traceWorld( TraceFarField, "ScreenProbes: Trace Far Field",
                    compact( "far field", CompactPush{ std::numeric_limits<float>::max(), 1u } ) );

        // SPATIAL FILTER (UE FilterScreenProbes, SpatialFilterNumPasses): the disocclusion of every probe's tile,
        // then three passes across neighbouring probes, each reading the last one's radiance.
        RDG::TextureRef radiance = traced;
        if ( inputs.Denoise )
        {
            const RDG::BufferRef disocclusion =
                 graph.CreateBuffer( RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) },
                                     "ScreenProbes.Disocclusion" );
            {
                ComputePipeline* const pipeline = m_Pipelines[Disocclusion].get();
                graph.AddPass(
                     "ScreenProbes: Disocclusion", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& builder )
                     {
                         auto block = builder.Bindings( m_Layouts[Disocclusion].Get( m_Shaders[Disocclusion] ),
                                                        Renderer::GetPipelineRouteFill( *pipeline ) );
                         block.Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbeParamsUB,
                                        paramsBuffer, kR )
                              .Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbeIndexUB, index,
                                        kR )
                              .Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbesUB, probes, kR )
                              .Storage( ScreenProbeGatherDisocclusionShaderParameters::GIFrameUB, inputs.GIFrame,
                                        kR )
                              .Texture( ScreenProbeGatherDisocclusionShaderParameters::Velocity, inputs.Velocity,
                                        kS, all )
                              .Sampler( ScreenProbeGatherDisocclusionShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( ScreenProbeGatherDisocclusionShaderParameters::GBufferShadingWord,
                                        inputs.GBufferShadingWord, kS, all )
                              .Sampler( ScreenProbeGatherDisocclusionShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( ScreenProbeGatherDisocclusionShaderParameters::DiffuseIndirectHistory,
                                        historyOrBlack( History::DiffuseIndirect ), kS, all )
                              .Sampler( ScreenProbeGatherDisocclusionShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( ScreenProbeGatherDisocclusionShaderParameters::HistoryGuide,
                                        historyOrBlack( History::Guide ), kS, all )
                              .Sampler( ScreenProbeGatherDisocclusionShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbeDisocclusionUB,
                                        disocclusion, kW );
                         gbuf( block );
                     },
                     [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                     } );
            }
            const uint32_t         res = layout.TracedTexels;
            const RDG::TextureDesc atlasDesc{
                 .Size   = { .Width = layout.AtlasProbesRow * res, .Height = layout.AtlasRows * res },
                 .Format = Core::Formats::ImageFormat::RGBA16F };
            for ( uint32_t pass = 0; pass < kSpatialFilterPasses; ++pass )
            {
                ComputePipeline* const pipeline = m_Pipelines[SpatialFilter].get();
                const RDG::TextureRef  input    = radiance;
                const RDG::TextureRef  output   = graph.CreateTexture( atlasDesc, "ScreenProbes.SpatialFiltered" );
                graph.AddPass(
                     "ScreenProbes: Spatial Filter", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& builder )
                     {
                         builder
                              .Bindings( m_Layouts[SpatialFilter].Get( m_Shaders[SpatialFilter] ),
                                         Renderer::GetPipelineRouteFill( *pipeline ) )
                              .Storage( ScreenProbeGatherSpatialFilterShaderParameters::ScreenProbeParamsUB,
                                        paramsBuffer, kR )
                              .Storage( ScreenProbeGatherSpatialFilterShaderParameters::ScreenProbeIndexUB, index,
                                        kR )
                              .Storage( ScreenProbeGatherSpatialFilterShaderParameters::ScreenProbesUB, probes,
                                        kR )
                              .Storage( ScreenProbeGatherSpatialFilterShaderParameters::ScreenProbeDisocclusionUB,
                                        disocclusion, kR )
                              .Texture( ScreenProbeGatherSpatialFilterShaderParameters::ScreenProbeRadiance, input,
                                        kS, all )
                              .Sampler( ScreenProbeGatherSpatialFilterShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Storage(
                                   ScreenProbeGatherSpatialFilterShaderParameters::FilteredScreenProbeRadiance,
                                   output, kW );
                     },
                     [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                     } );
                radiance = output;
            }
        }
        {
            ComputePipeline* const pipeline = m_Pipelines[Filter].get();
            graph.AddPass(
                 "ScreenProbes: Filter", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Filter].Get( m_Shaders[Filter] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeParamsUB, paramsBuffer,
                                    kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Texture( ScreenProbeGatherFilterShaderParameters::ScreenProbeRadiance, radiance, kS,
                                    all )
                          .Sampler( ScreenProbeGatherFilterShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeFilteredTexture, filtered,
                                    kW )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeSHUB, sh, kW )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeBackfaceUB,
                                    backfaceIrradiance, kW )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeTraceHitUB, traceHit, kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::ScreenProbeMovingUB, moving, kW )
                          .Storage( ScreenProbeGatherFilterShaderParameters::RadianceCacheParamsUB,
                                    inputs.RadianceCache.Params, kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::RadianceCacheIndirectionUB,
                                    inputs.RadianceCache.Indirection, kR )
                          .Storage( ScreenProbeGatherFilterShaderParameters::RadianceCacheProbesUB,
                                    inputs.RadianceCache.Probes, kR )
                          .Texture( ScreenProbeGatherFilterShaderParameters::RadianceCacheIrradianceTexture,
                                    inputs.RadianceCache.Irradiance, kS, all )
                          .Sampler( ScreenProbeGatherFilterShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( ScreenProbeGatherFilterShaderParameters::RadianceCacheDepthTexture,
                                    inputs.RadianceCache.Depth, kS, all )
                          .Sampler( ScreenProbeGatherFilterShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() );
                 },
                 [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }
        {
            ComputePipeline* const pipeline = m_Pipelines[Integrate].get();
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
            graph.AddPass(
                 "ScreenProbes: Integrate", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Integrate].Get( m_Shaders[Integrate] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeParamsUB, paramsBuffer,
                                    kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeSHUB, sh, kR )
                          .Texture( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeFilteredTexture,
                                    filtered, kS, all )
                          .Sampler( ScreenProbeGatherIntegrateShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeDiffuseOutTexture,
                                    diffuse, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeSpecularOutTexture,
                                    specular, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeBackfaceUB,
                                    backfaceIrradiance, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeBackfaceOutTexture,
                                    backface, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeMovingUB, moving, kR )
                          // A pixel no screen probe holds reads the world radiance cache in place.
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::RadianceCacheParamsUB,
                                    inputs.RadianceCache.Params, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::RadianceCacheIndirectionUB,
                                    inputs.RadianceCache.Indirection, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::RadianceCacheProbesUB,
                                    inputs.RadianceCache.Probes, kR )
                          .Texture( ScreenProbeGatherIntegrateShaderParameters::RadianceCacheIrradianceTexture,
                                    inputs.RadianceCache.Irradiance, kS, all )
                          .Sampler( ScreenProbeGatherIntegrateShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( ScreenProbeGatherIntegrateShaderParameters::RadianceCacheDepthTexture,
                                    inputs.RadianceCache.Depth, kS, all )
                          .Sampler( ScreenProbeGatherIntegrateShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( ScreenProbeGatherIntegrateShaderParameters::ShortRangeAoTexture, shortRangeAO,
                                    kS, all )
                          .Sampler( ScreenProbeGatherIntegrateShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        if ( !inputs.Denoise )
            return Common::MakeSuccess( ScreenProbeGatherFrame{ .Diffuse      = diffuse,
                                                                .Specular     = specular,
                                                                .Backface     = backface,
                                                                .ShortRangeAO = inputs.ShortRangeAO } );

        // TEMPORAL FILTER (UE ScreenProbeTemporalReprojectionCS): this frame's lighting blended with the view's
        // history into the view's new history set, which is also what the composite reads.
        const History::Refs current = history.CreateCurrent( graph, extent );
        {
            ComputePipeline* const pipeline = m_Pipelines[Temporal].get();
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
            history.SetWriterPass( graph.GetPassCount() );
            graph.AddPass(
                 "ScreenProbes: Temporal", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     using H    = History;
                     auto block = builder.Bindings( m_Layouts[Temporal].Get( m_Shaders[Temporal] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherTemporalShaderParameters::ScreenProbeParamsUB, paramsBuffer,
                                    kR )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::Velocity, inputs.Velocity, kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::GBufferShadingWord,
                                    inputs.GBufferShadingWord, kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::DiffuseIndirect, diffuse, kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::RoughSpecularIndirect, specular, kS,
                                    all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::BackfaceDiffuseIndirect, backface,
                                    kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::DiffuseIndirectHistory,
                                    historyOrBlack( H::DiffuseIndirect ), kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::RoughSpecularIndirectHistory,
                                    historyOrBlack( H::RoughSpecularIndirect ), kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::BackfaceDiffuseIndirectHistory,
                                    historyOrBlack( H::BackfaceDiffuseIndirect ), kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::HistoryGuide,
                                    historyOrBlack( H::Guide ), kS, all )
                          .Sampler( ScreenProbeGatherTemporalShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::NewHistoryDiffuseIndirect,
                                    current[H::DiffuseIndirect], kW )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::NewHistoryRoughSpecularIndirect,
                                    current[H::RoughSpecularIndirect], kW )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::NewHistoryBackfaceDiffuseIndirect,
                                    current[H::BackfaceDiffuseIndirect], kW )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::NewHistoryGuide, current[H::Guide],
                                    kW );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }
        return Common::MakeSuccess( ScreenProbeGatherFrame{ .Diffuse  = current[History::DiffuseIndirect],
                                                            .Specular = current[History::RoughSpecularIndirect],
                                                            .Backface = current[History::BackfaceDiffuseIndirect],
                                                            .ShortRangeAO = inputs.ShortRangeAO } );
    }
} // namespace Desert::Graphic::GlobalIllumination

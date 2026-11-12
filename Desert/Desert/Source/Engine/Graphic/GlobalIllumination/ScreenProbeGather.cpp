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
#include <cstring>
#include <format>
#include <limits>
#include <optional>
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
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeCompactAllocatorUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeCompactTexelsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOutArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOutAllocatorUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOutTexelsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut2ArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut2AllocatorUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut2TexelsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut3ArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut3AllocatorUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeOut3TexelsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeNearHitUB );
#if DESERT_DEV_INSTRUMENTS
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeMissReasonsUB );
#endif
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
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIntegrateArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIntegrateTilesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIntegrateUncoveredArgsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIntegrateUncoveredUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeIntegrateUncoveredMaskUB );
    };
    struct ScreenProbeGatherDisocclusionShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWord );
        DESERT_SHADER_PARAMETER( Texture, DiffuseIndirectHistory );
        DESERT_SHADER_PARAMETER( Texture, HistoryGuide );
        DESERT_SHADER_PARAMETER( StorageBuffer, ScreenProbeDisocclusionUB );
        DESERT_SHADER_PARAMETER( StorageTexture, ScreenProbeHistoryTaps );
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
        DESERT_SHADER_PARAMETER( Texture, ScreenProbeHistoryTaps );
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

        constexpr std::array<std::string_view, 17> kScreenProbeShaderNames{
             "ScreenProbePlace",         "ScreenProbeShortRangeAO", "ScreenProbeTraceScreen",
             "ScreenProbeTrace",         "ScreenProbeTrace",        "ScreenProbeTrace",
             "ScreenProbeTrace",         "ScreenProbeTrace",        "ScreenProbeTrace",
             "ScreenProbeDisocclusion",
             "ScreenProbeSpatialFilter", "ScreenProbeFilter",       "ScreenProbeIntegrate",
             "ScreenProbeIntegrate",     "ScreenProbeIntegrate",    "ScreenProbeIntegrate",
             "ScreenProbeTemporal" };

        // The permutation define of a layout program that shares its shader with another (empty: none); the
        // integrate's tile classification, its simple-diffuse permutation and its uncovered-pixel pass
        // (ScreenProbeIntegrate.shader).
        constexpr std::array<std::string_view, kScreenProbeShaderNames.size()> kScreenProbeProgramDefines{
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "",
             "SP_INTEGRATE_CLASSIFY",
             "SP_INTEGRATE_SIMPLE_DIFFUSE",
             "",
             "SP_INTEGRATE_UNCOVERED",
             "" };

        // ScreenProbeIntegrateArgsUB: the simple-diffuse tiles' dispatch, then the full tiles' (x, y = a 2D grid
        // of SP_INTEGRATE_GROUPS_X groups a row, z = 1, w = the list's count), counted by the classification.
        // Static storage: QueueStaticBufferUpload copies nothing.
        constexpr std::array<uint32_t, 8> kIntegrateArgsReset{ 0u, 0u, 1u, 0u, 0u, 0u, 1u, 0u };
        constexpr uint64_t                kIntegrateFullArgsOffset = 4u * sizeof( uint32_t );
        // ScreenProbeIntegrateUncoveredArgsUB: the uncovered tiles' dispatch (x, y = the same 2D grid, z = 1,
        // w = the list's count), listed by both integrate permutations (THE UNCOVERED PIXELS,
        // ScreenProbeIntegrate.shader). The mask: SP_UNCOVERED_MASK_WORDS words a tile, zeroed by the
        // classification.
        constexpr std::array<uint32_t, 4> kIntegrateUncoveredArgsReset{ 0u, 0u, 1u, 0u };
        constexpr uint64_t                kIntegrateUncoveredMaskWords = 3u;

        // UE r.Lumen.ScreenProbeGather.SpatialFilterNumPasses.
        constexpr uint32_t kSpatialFilterPasses = 3;

        // The defines of the world legs' three permutations of ScreenProbeTrace (besides TracerVariant's).
        constexpr std::string_view kTraceNearFieldDefine = "SP_TRACE_NEAR_FIELD";
        constexpr std::string_view kTraceFarFieldDefine  = "SP_TRACE_FAR_FIELD";
        constexpr std::string_view kTraceCacheMissDefine = "SP_TRACE_CACHE_MISS";
        constexpr std::string_view kTraceHitLightingDefine = "SP_TRACE_HIT_LIGHTING";
        constexpr std::string_view kTraceCacheReadDefine   = "SP_TRACE_CACHE_READ";
        constexpr std::string_view kTraceNearHitLightingDefine = "SP_TRACE_NEAR_HIT_LIGHTING";

        // Whether a tracer has the leg at all: the two hit lightings are the software tracer's deferred passes (its
        // far field defers global-field hits, its near field every mesh SDF hit - FPS-NEAR-FIELD); the hardware
        // legs light their triangle hits in place, so either leg would be an empty dispatch over an empty list.
        [[nodiscard]] constexpr bool TraceLegExists( const Tracer tracer, const std::string_view leg )
        {
            if ( leg == kTraceHitLightingDefine || leg == kTraceNearHitLightingDefine )
                return tracer == Tracer::SoftwareDistanceField;
            return true;
        }

        // Whether the leg lights a hit (ScreenProbeTrace.shader SP_TRACE_LIGHTS_HITS - the same rule): only such a
        // leg declares PointClampSampler, the G-buffer textures and (GIHitLighting.glslh) the surface-cache reads,
        // so the pass binds them exactly then (UE: a pass binds the parameters its permutation declares).
        [[nodiscard]] constexpr bool TraceLegLightsHits( const Tracer tracer, const std::string_view leg )
        {
            if ( leg == kTraceCacheMissDefine )
                return true;
            if ( leg == kTraceHitLightingDefine || leg == kTraceNearHitLightingDefine )
                return tracer == Tracer::SoftwareDistanceField;
            if ( leg == kTraceNearFieldDefine ) // the software near field defers its hits (SP_TRACE_NEAR_HIT_LIGHTING)
                return tracer != Tracer::SoftwareDistanceField;
            if ( leg == kTraceFarFieldDefine )
                return tracer != Tracer::SoftwareDistanceField;
            return false; // SP_TRACE_CACHE_READ: the radiance cache only
        }

        // A trace leg's list (THE LEGS' LISTS, Common/ScreenProbeTracing.glslh): its dispatch x = groups, y = z =
        // 1 (a dispatch's three counts; w unused), counted up by the pass that lists the rays; the allocator: 0.
        constexpr std::array<uint32_t, 4> kCompactArgsReset{ 0u, 1u, 1u, 0u };
        constexpr uint32_t                kCompactAllocatorReset = 0u;

        // The near field's end along a ray (ScreenProbeTraceScreen's NearFieldEnd: the screen lists a ray past it
        // for the far field): the software tracer's mesh SDFs trace up to DF_MESH_SDF_TRACE_DISTANCE
        // (GITraceNearField); a ray the screen took past it has nothing left there.
        [[nodiscard]] float NearFieldEnd( const Tracer tracer )
        {
            return tracer == Tracer::SoftwareDistanceField ? DistanceField::DFShared::DF_MESH_SDF_TRACE_DISTANCE
                                                           : std::numeric_limits<float>::max();
        }

        // ScreenProbeTraceScreen's push constant.
        struct TraceScreenPush
        {
            float NearFieldEnd;
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

    std::string_view ScreenProbeGather::TraceLegDefine( const Program program )
    {
        switch ( program )
        {
            case TraceNearField:
                return kTraceNearFieldDefine;
            case TraceFarField:
                return kTraceFarFieldDefine;
            case TraceCacheMiss:
                return kTraceCacheMissDefine;
            case TraceHitLighting:
                return kTraceHitLightingDefine;
            case TraceCacheRead:
                return kTraceCacheReadDefine;
            case TraceNearHitLighting:
                return kTraceNearHitLightingDefine;
            default:
                return {}; // not a trace leg: no SP_TRACE_* permutation
        }
    }

    namespace
    {
        // The layout permutation every screen-probe program is compiled under (Common/ScreenProbes.glslh
        // SP_DOWNSAMPLE_LOG2 / SP_TRACED_TEXELS): its groupshared arrays and per-probe workgroups are SP_RES² wide.
        std::vector<std::string> ScreenProbeLayoutDefines( const uint32_t downsample, const uint32_t tracedTexels )
        {
            return { std::format( "SP_DOWNSAMPLE_LOG2={}", std::countr_zero( downsample ) ),
                     std::format( "SP_TRACED_TEXELS={}", tracedTexels ) };
        }

        // A layout program's permutation: the layout's defines, then its own (kScreenProbeProgramDefines); cooked
        // (ProgramVariants).
        Core::ShaderVariant LayoutVariant( const uint32_t downsample, const uint32_t tracedTexels,
                                           const std::string_view permutation )
        {
            Core::ShaderVariant variant{ .Defines       = ScreenProbeLayoutDefines( downsample, tracedTexels ),
                                         .ShippedByCook = true };
            if ( !permutation.empty() )
                variant.Defines.emplace_back( permutation );
            return variant;
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

#if DESERT_DEV_INSTRUMENTS
        // PROBE-TURN-1: the cache-read leg - the one that decides a cache miss (GPU-FARFIELD-6; the far field's
        // before) - counts every radiance cache read by why it answered or not, the far-field and hit-lighting
        // legs their ray classes (ScreenProbeTrace.shader SP_TRACE_MISS_REASONS, ScreenProbes.MissReasons). Dev
        // builds compile them in that permutation; the cook ships both.
        constexpr std::string_view kTraceMissReasonsDefine = "SP_TRACE_MISS_REASONS";

        Core::ShaderVariant CountedTraceVariant( const Pipeline& pipeline, const std::string_view leg,
                                                 const uint32_t downsample, const uint32_t tracedTexels )
        {
            Core::ShaderVariant variant = TraceVariant( pipeline, leg, downsample, tracedTexels );
            if ( leg == kTraceFarFieldDefine || leg == kTraceHitLightingDefine || leg == kTraceCacheReadDefine )
                variant.Defines.emplace_back( kTraceMissReasonsDefine );
            return variant;
        }

        // ScreenProbes.MissReasons, zeroed per frame: one word per RADIANCE_CACHE_READ_* (Common/RadianceCache.glslh).
        constexpr std::array<uint32_t, ScreenProbeMissReasons::kWords> kMissReasonsReset{};
#endif
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
            return RefuseLayout( downsample, tracedTexels, {}, {}, false,
                                 std::format( "screen probes: the layout {} px / {} texels is not one the shaders "
                                              "compile (8, 16, 32 or 64 px a probe; 4..16 traced texels a side)",
                                              downsample, tracedTexels ) );
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( uint32_t program = 0; program < ProgramCount; ++program )
        {
            if ( std::ranges::find( kTracePrograms, static_cast<Program>( program ) ) != kTracePrograms.end() )
                continue; // tracer permutations, EnsureTraceProgram
            const std::string         name( kScreenProbeShaderNames[program] );
            const std::string_view    permutation = kScreenProbeProgramDefines[program];
            const Core::ShaderVariant variant     = LayoutVariant( downsample, tracedTexels, permutation );
            auto                      acquired    = shaders->AcquireVariant( name, variant );
            if ( !acquired )
                return RefuseLayout( downsample, tracedTexels, name, permutation, true,
                                     std::format( "screen probes: shader {} {}({} px, {} texels) was refused: {}",
                                                  name, permutation, downsample, tracedTexels,
                                                  acquired.GetError() ) );
            auto shader   = acquired.ExtractValue();
            auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !pipeline )
                return RefuseLayout( downsample, tracedTexels, name, permutation, false,
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
                                                           std::string program, const std::string_view permutation,
                                                           const bool service, std::string error )
    {
        // The same refusal asked again keeps its Reported; a different layout or program is a new one.
        if ( !m_LayoutRefusal || m_LayoutRefusal->Downsample != downsample ||
             m_LayoutRefusal->TracedTexels != tracedTexels || m_LayoutRefusal->Program != program ||
             m_LayoutRefusal->Permutation != permutation || m_LayoutRefusal->Service != service )
            m_LayoutRefusal = LayoutRefusal{ .Downsample   = downsample,
                                             .TracedTexels = tracedTexels,
                                             .Program      = std::move( program ),
                                             .Permutation  = permutation,
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
                 LayoutVariant( m_LayoutRefusal->Downsample, m_LayoutRefusal->TracedTexels,
                                m_LayoutRefusal->Permutation ) );
        return !std::exchange( m_LayoutRefusal->Reported, true );
    }

#if DESERT_DEV_INSTRUMENTS
    void ScreenProbeMissCounts::AddFrame( RDG::Builder& graph, const uint64_t frameIndex,
                                          const RDG::BufferRef counts )
    {
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        RDG::ExternalBuffer& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        // The words this slot was handed slots-in-flight frames ago: its fence has passed.
        if ( slot.Physical )
        {
            const std::span<const std::byte> bytes = slot.Physical->ReadForHost();
            if ( bytes.size() >= sizeof( m_Last.Rays ) )
            {
                std::memcpy( m_Last.Rays.data(), bytes.data(), sizeof( m_Last.Rays ) );
                m_Last.Valid = true;
            }
        }
        slot = RDG::ExternalBuffer{};
        graph.Extract( counts, slot, RDG::Access::HostRead );
    }
#endif

    Common::BoolResultStr ScreenProbeGather::EnsureTraceProgram( const Pipeline& pipeline )
    {
        bool current = pipeline == m_TraceProgramFor && m_TraceProgramDownsample == m_ProgramDownsample &&
                       m_TraceProgramTracedTexels == m_ProgramTracedTexels;
        for ( const Program program : kTracePrograms )
            if ( TraceLegExists( pipeline.Tracer, TraceLegDefine( program ) ) && !m_Pipelines[program] )
                current = false;
        if ( current )
            return BOOLSUCCESS;
        for ( const Program program : kTracePrograms )
        {
            const std::string_view leg = TraceLegDefine( program );
            if ( !TraceLegExists( pipeline.Tracer, leg ) )
            {
                // the tracer has no such leg: no program, nothing dispatched (Execute)
                m_Shaders[program].reset();
                m_Pipelines[program].reset();
                continue;
            }
            const std::string name( kScreenProbeShaderNames[program] );
#if DESERT_DEV_INSTRUMENTS
            const Core::ShaderVariant variant =
                 CountedTraceVariant( pipeline, leg, m_ProgramDownsample, m_ProgramTracedTexels );
#else
            const Core::ShaderVariant variant = TraceVariant( pipeline, leg, m_ProgramDownsample, m_ProgramTracedTexels );
#endif
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
            if ( std::ranges::find( kTracePrograms, static_cast<Program>( program ) ) == kTracePrograms.end() )
                variants.push_back(
                     { std::string( kScreenProbeShaderNames[program] ),
                       LayoutVariant( downsample, tracedTexels, kScreenProbeProgramDefines[program] ) } );
        for ( const Pipeline& tracer : kTracerPermutations )
            for ( const Program program : kTracePrograms )
            {
                const std::string_view leg = TraceLegDefine( program );
                if ( !TraceLegExists( tracer.Tracer, leg ) )
                    continue; // the leg EnsureTraceProgram never acquires for this tracer
                variants.push_back( { std::string( kScreenProbeShaderNames[program] ),
                                      TraceVariant( tracer, leg, downsample, tracedTexels ) } );
#if DESERT_DEV_INSTRUMENTS
                if ( leg == kTraceFarFieldDefine || leg == kTraceHitLightingDefine ||
                     leg == kTraceCacheReadDefine )
                    variants.push_back( { std::string( kScreenProbeShaderNames[program] ),
                                          CountedTraceVariant( tracer, leg, downsample, tracedTexels ) } );
#endif
            }
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
        // Half the G-buffer's resolution, rounded up (UE r.Lumen.ScreenProbeGather.ShortRangeAO.Downsample).
        m_ShortRangeAO = CreateTarget( "ScreenProbes.ShortRangeAO", DivideRoundUp( extent.x, 2u ),
                                       DivideRoundUp( extent.y, 2u ) );
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
        // ASYNC COMPUTE (UE Lumen screen probes on the async pipe, ERDGPassFlags::AsyncCompute): every node below
        // is a compute dispatch whose every input and output is a graph resource (G-buffer, HZB, velocity, the
        // radiance cache, the tracer's field / TLAS, the surface cache), so the graph forks at the last graphics
        // writer of what the run reads (the G-buffer, HZB) and joins at the first graphics reader of its lighting
        // (Deferred: Composite). Four nodes are the graphics queue's (the short-range AO is not: FPS-FIREFLY):
        // the placement and the screen leg (below, FPS-ASYNC-JOIN2), the disocclusion and the integrate
        // classification (below, FPS-COMPOSITE-WAIT) - the passes the radiance cache does not gate, run beside the
        // cache instead of queued behind it.
        const RDG::PassFlags kAsyncPass    = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        const RDG::PassFlags kGraphicsPass = RDG::PassFlags::Compute;
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
             RDG::BufferDesc{ layout.Probes() * ScreenProbeLayout::kSHPackedVec4 * 16ull }, "ScreenProbes.SH" );
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
        // ON THE GRAPHICS QUEUE, with the screen leg (FPS-ASYNC-JOIN2). The async queue is one FIFO: the radiance
        // cache (Mark .. Filter, ~0.9 ms on FPS-FIELD-BARN-TURN scn-1112-151554) is submitted before the gather,
        // so on AsyncCompute the placement and the screen leg ran only after it, although neither reads anything
        // the cache writes (the placement: the queued params / GI frame, velocity, the G-buffer; the screen leg:
        // those, the HZB, the scene colour history and the cache's queued params - every one a resource Graphics
        // holds, RDGCompile FindAsyncRuns). The first leg that reads the cache is the near field (its indirection
        // and probes). The chain the composite waited for (1.03 ms "RDG Wait .. before 'Deferred: Composite'")
        // was cache + placement + screen + near .. temporal in a row; with the head on Graphics it is cache +
        // near .. temporal, and the head runs on Graphics in the same window (UE overlaps the cache update and the
        // screen trace the same way: the cache's own dispatches and the screen traces do not wait on each other).
        // The near field's piece forks after the screen leg (CutRunsAtConsumers / the fork at its producers), long
        // before the cache ends. The same dispatches, inputs and outputs: the image is unchanged.
        for ( uint32_t level = 0; level < layout.Levels; ++level )
        {
            ComputePipeline* const pipeline = m_Pipelines[Place].get();
            const uint32_t         spacing  = layout.Downsample >> level;
            const glm::uvec2       groups( DivideRoundUp( DivideRoundUp( inputs.Extent.x, spacing ), 8u ),
                                           DivideRoundUp( DivideRoundUp( inputs.Extent.y, spacing ), 8u ) );
            graph.AddPass(
                 kPlaceNames[level], kGraphicsPass,
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
        // ON THE ASYNC PIPE, as UE dispatches it with the rest of the gather (FPS-FIREFLY). FPS-ASYNC-JOIN had
        // moved it to the graphics queue (51fc781b1); from that build on FIELD_Barn shows a bloomed red-orange
        // firefly in the grass in about half the runs (scn-1112-153600 on), and of the two merges of that build
        // the other (FPS-DEVLOST11 grass) changes no GPU work in that scene (its repeated-key branch never fires
        // there). The AO's declared uses (params, GI frame, G-buffer depth / normal, its target) give the graph
        // every edge, and the compile's fork / join / ownership / aliasing walk for this shape was read without
        // finding the missing order (REMAINDER-FPS-FIREFLY-1), so the AO goes back to the pipe its consumers run
        // on until that order is found: the integrate reads it on the same queue, after it in submission order.
        if ( inputs.ShortRangeAO )
        {
            ComputePipeline* const pipeline = m_Pipelines[ShortRangeAO].get();
            // One thread per half-resolution texel (ScreenProbes.ShortRangeAO's extent).
            const glm::uvec2 groups( DivideRoundUp( DivideRoundUp( inputs.Extent.x, 2u ), 8u ),
                                     DivideRoundUp( DivideRoundUp( inputs.Extent.y, 2u ), 8u ) );
            graph.AddPass(
                 "ScreenProbes: ShortRangeAO", kAsyncPass,
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

        // TRACE (UE LumenScreenProbeTracing.cpp TraceScreenProbes): the screen; the near field over the rays it
        // left before the near field's end; the far field over every ray still without a hit, out to the radiance
        // cache's coverage and the sky; the cache read, the hit lighting and the cache miss over the rays the far
        // field and the cache read left them. Each ray's state between the legs: the trace hit buffer; each leg's
        // list: written by the pass that left its rays (THE LEGS' LISTS, Common/ScreenProbeTracing.glslh), so no
        // pass rescans the rays (UE's compaction passes, folded in).
        const uint32_t       probeGroups = layout.Probes();
        const uint32_t       kTexels     = layout.TracedTexels * layout.TracedTexels;
        const uint64_t       rayBytes    = static_cast<uint64_t>( probeGroups ) * kTexels * sizeof( uint32_t );
        const RDG::BufferRef traceHit = graph.CreateBuffer( RDG::BufferDesc{ rayBytes }, "ScreenProbes.TraceHit" );
#if DESERT_DEV_INSTRUMENTS
        const RDG::BufferRef missReasons =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( kMissReasonsReset ) }, "ScreenProbes.MissReasons" );
        graph.QueueBufferUpload( missReasons, std::as_bytes( std::span( kMissReasonsReset ) ) );
#endif
        // One leg's list and its dispatch arguments.
        struct RayList
        {
            RDG::BufferRef Args;      // the leg's dispatch (IndirectArgs)
            RDG::BufferRef Allocator; // the rays listed
            RDG::BufferRef Texels;    // probe * SP_RES² + texel
        };
        const auto rayList = [&]( const std::string_view name ) -> RayList
        {
            const RayList list{ graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactArgsReset ) },
                                                    std::format( "ScreenProbes.ListArgs ({})", name ) ),
                                graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactAllocatorReset ) },
                                                    std::format( "ScreenProbes.ListAllocator ({})", name ) ),
                                graph.CreateBuffer( RDG::BufferDesc{ rayBytes },
                                                    std::format( "ScreenProbes.ListTexels ({})", name ) ) };
            graph.QueueBufferUpload( list.Args, std::as_bytes( std::span( kCompactArgsReset ) ) );
            graph.QueueBufferUpload( list.Allocator, std::as_bytes( std::span( &kCompactAllocatorReset, 1 ) ) );
            return list;
        };
        const RayList nearList      = rayList( "near field" );
        const RayList farList       = rayList( "far field" );
        const RayList cacheReadList = rayList( "cache read" );
        const RayList cacheMissList = rayList( "cache miss" );
        // The hardware far field lights its hits in place: no hit-lighting leg (TraceLegExists), no list.
        std::optional<RayList> hitLightingList;
        if ( TraceLegExists( inputs.Tracer.Pipeline.Tracer, kTraceHitLightingDefine ) )
            hitLightingList = rayList( "hit lighting" );
        // FPS-NEAR-FIELD: the software near field's hits (UE TraceMeshSDFs leaves its hits to the hit lighting
        // pass) - their list, and each listed ray's hit object and exact distance (ScreenProbes.NearHit, two uints
        // a ray, indexed like the trace hit buffer).
        std::optional<RayList> nearHitLightingList;
        RDG::BufferRef         nearHit;
        if ( TraceLegExists( inputs.Tracer.Pipeline.Tracer, kTraceNearHitLightingDefine ) )
        {
            nearHitLightingList = rayList( "near hit lighting" );
            nearHit = graph.CreateBuffer( RDG::BufferDesc{ rayBytes * 2u }, "ScreenProbes.NearHit" );
        }

        // A pass's first, second and third output lists (ScreenProbeOut* / ScreenProbeOut2* / ScreenProbeOut3*).
        const auto bindOut = [&]( auto& block, const RayList& list )
        {
            block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOutArgsUB, list.Args, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOutAllocatorUB, list.Allocator, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOutTexelsUB, list.Texels, kW );
        };
        const auto bindOut2 = [&]( auto& block, const RayList& list )
        {
            block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut2ArgsUB, list.Args, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut2AllocatorUB, list.Allocator, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut2TexelsUB, list.Texels, kW );
        };
        const auto bindOut3 = [&]( auto& block, const RayList& list )
        {
            block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut3ArgsUB, list.Args, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut3AllocatorUB, list.Allocator, kW )
                 .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeOut3TexelsUB, list.Texels, kW );
        };

        {
            ComputePipeline* const pipeline = m_Pipelines[TraceScreen].get();
            const TraceScreenPush  push{ NearFieldEnd( inputs.Tracer.Pipeline.Tracer ) };
            graph.AddPass(
                 "ScreenProbes: Trace Screen", kGraphicsPass,
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
                                    RDG::SamplerDesc::PointClamp() )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( TraceScreenPush ) ) );
                     // The near field's list, the far field's (the screen's misses past the near field) and the
                     // cache read's (FPS-SP-TRACE-4: the misses it walked to the cache have nothing left to
                     // march).
                     bindOut( block, nearList );
                     bindOut2( block, farList );
                     bindOut3( block, cacheReadList );
                 },
                 [pipeline, probeGroups, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }

        // HISTORY AND INTEGRATION CLASSIFICATION ON THE GRAPHICS QUEUE (FPS-COMPOSITE-WAIT), beside the radiance
        // cache like the placement, the short-range AO and the screen leg (FPS-ASYNC-JOIN / -JOIN2). The async
        // queue is one FIFO and the composite joins the end of radiance cache (~0.9 ms on FIELD_Barn TURN) + near
        // .. temporal (~2.1 ms), a chain every pass of which reads the cache or the traced radiance - except these
        // two: the disocclusion reads the queued params, the GI frame, the velocity, the G-buffer and the view's
        // history; the integrate classification the GI frame and the G-buffer normal - all of it held by Graphics.
        // On AsyncCompute they lengthened the chain the composite waited for (0.71-0.85 ms "RDG Wait .. before
        // 'Deferred: Composite (Simple Tiles)'", scn-1112-195853) by their 0.12 + 0.04 ms; here Graphics runs them
        // while the async queue still runs the cache, and the spatial filter's / the integrate's pieces read what
        // they wrote (a graphics producer before the near field's fork, so no new fork). The same dispatches,
        // inputs and outputs in the same order of their readers: the image is unchanged.
        RDG::BufferRef  disocclusion;
        RDG::TextureRef historyTaps;
        if ( inputs.Denoise )
        {
            disocclusion = graph.CreateBuffer(
                 RDG::BufferDesc{ static_cast<uint64_t>( layout.UniformProbes ) * sizeof( float ) },
                 "ScreenProbes.Disocclusion" );
            historyTaps = graph.CreateTexture(
                 RDG::TextureDesc{ .Size   = { .Width = inputs.Extent.x, .Height = inputs.Extent.y },
                                   .Format = Core::Formats::ImageFormat::RG32_UINT },
                 "ScreenProbes.HistoryTaps" );
            {
                // One group per uniform tile, from the strip before the jittered grid (tile -1) to past the frame.
                const glm::uvec2       classifyGroups( DivideRoundUp( inputs.Extent.x, layout.Downsample ) + 1u,
                                                       DivideRoundUp( inputs.Extent.y, layout.Downsample ) + 1u );
                ComputePipeline* const pipeline = m_Pipelines[Disocclusion].get();
                graph.AddPass(
                     "ScreenProbes: Disocclusion", kGraphicsPass,
                     [&]( RDG::PassBuilder& builder )
                     {
                         auto block = builder.Bindings( m_Layouts[Disocclusion].Get( m_Shaders[Disocclusion] ),
                                                        Renderer::GetPipelineRouteFill( *pipeline ) );
                         block.Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbeParamsUB,
                                        paramsBuffer, kR )
                              .Storage( ScreenProbeGatherDisocclusionShaderParameters::ScreenProbeHistoryTaps,
                                        historyTaps, kW )
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
                     [pipeline, classifyGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *pipeline, classifyGroups.x, classifyGroups.y,
                                                           1u );
                     } );
            }
        }
        // INTEGRATION TILE CLASSIFICATION (UE r.Lumen.ScreenProbeGather.IntegrationTileClassification): every 8x8
        // tile listed simple-diffuse or full (ScreenProbeIntegrate.shader SP_INTEGRATE_CLASSIFY), then the two
        // integrate permutations dispatched indirectly over their own lists.
        using IntegrateSlots = ScreenProbeGatherIntegrateShaderParameters;
        const glm::uvec2     integrateTiles( DivideRoundUp( inputs.Extent.x, 8u ),
                                             DivideRoundUp( inputs.Extent.y, 8u ) );
        const RDG::BufferRef integrateArgs =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( kIntegrateArgsReset ) }, "ScreenProbes.IntegrateArgs" );
        graph.QueueStaticBufferUpload( integrateArgs, std::as_bytes( std::span( kIntegrateArgsReset ) ) );
        const RDG::BufferRef integrateTileList = graph.CreateBuffer(
             RDG::BufferDesc{ static_cast<uint64_t>( integrateTiles.x ) * integrateTiles.y * sizeof( uint32_t ) },
             "ScreenProbes.IntegrateTiles" );
        // THE UNCOVERED PIXELS (ScreenProbeIntegrate.shader): the tiles holding a pixel no probe holds, their
        // dispatch and the per-tile pixel masks.
        const uint64_t       integrateTileCount = static_cast<uint64_t>( integrateTiles.x ) * integrateTiles.y;
        const RDG::BufferRef uncoveredArgs      = graph.CreateBuffer(
             RDG::BufferDesc{ sizeof( kIntegrateUncoveredArgsReset ) }, "ScreenProbes.IntegrateUncoveredArgs" );
        graph.QueueStaticBufferUpload( uncoveredArgs, std::as_bytes( std::span( kIntegrateUncoveredArgsReset ) ) );
        const RDG::BufferRef uncoveredTiles = graph.CreateBuffer(
             RDG::BufferDesc{ integrateTileCount * sizeof( uint32_t ) }, "ScreenProbes.IntegrateUncoveredTiles" );
        const RDG::BufferRef uncoveredMask = graph.CreateBuffer(
             RDG::BufferDesc{ integrateTileCount * kIntegrateUncoveredMaskWords * sizeof( uint32_t ) },
             "ScreenProbes.IntegrateUncoveredMask" );
        {
            ComputePipeline* const pipeline = m_Pipelines[IntegrateClassify].get();
            graph.AddPass(
                 "ScreenProbes: Integrate Classify", kGraphicsPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block =
                          builder.Bindings( m_Layouts[IntegrateClassify].Get( m_Shaders[IntegrateClassify] ),
                                            Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( IntegrateSlots::GIFrameUB, inputs.GIFrame, kR )
                          .Texture( IntegrateSlots::GBufferBTexture, inputs.GBufferB, kS, all )
                          .Sampler( IntegrateSlots::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateArgsUB, integrateArgs, kW )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateTilesUB, integrateTileList, kW )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredMaskUB, uncoveredMask, kW );
                 },
                 [pipeline, integrateTiles]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, integrateTiles.x, integrateTiles.y,
                                                       1u );
                 } );
        }

        // One world leg over its list, one thread per ray (ScreenProbeTrace.shader's permutation), listing the
        // rays it leaves for the next legs in @p out (and @p out2: the far field's hits for the hit lighting, the
        // near field's misses at the cache for the cache read).
        const auto traceWorld = [&]( const Program program, const std::string_view name, const RayList& in,
                                     const RayList* const out, const RayList* const out2 )
        {
            ComputePipeline* const pipeline = m_Pipelines[program].get();
            // the declaration's own rule (ScreenProbeTrace.shader SP_TRACE_LIGHTS_HITS)
            const bool lightsHits = TraceLegLightsHits( inputs.Tracer.Pipeline.Tracer, TraceLegDefine( program ) );
            graph.AddPass(
                 name, kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[program].Get( m_Shaders[program] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     builder.Read( in.Args, RDG::Access::IndirectArgs );
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
                                    in.Allocator, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeCompactTexelsUB,
                                    in.Texels, kR )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeSpeedUB, speed, kR );
                     // The lists of the legs after it (ScreenProbeTrace.shader SP_TRACE_LISTS_RAYS / _HITS).
                     if ( out )
                         bindOut( block, *out );
                     if ( out2 )
                         bindOut2( block, *out2 );
                     // The software near field's hits (ScreenProbeTrace.shader SP_TRACE_DEFERS_NEAR_HITS) and the
                     // leg that lights them.
                     if ( program == TraceNearField && nearHitLightingList )
                     {
                         bindOut3( block, *nearHitLightingList );
                         block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeNearHitUB, nearHit, kW );
                     }
                     if ( program == TraceNearHitLighting )
                         block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeNearHitUB, nearHit, kR );
                     // The cache-read leg traces nothing (ScreenProbeTrace.shader includes no tracer); a leg that
                     // lights no hit samples nothing point-clamped (the object grid is fetched).
                     const TracerPointClamp pointClamp = lightsHits ? TracerPointClamp::Bind : TracerPointClamp::Skip;
                     if ( program != TraceCacheRead )
                         DeclareTracerReads( block, inputs.Tracer, kS, pointClamp );
                     if ( readsCards && lightsHits )
                         DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
#if DESERT_DEV_INSTRUMENTS
                     if ( program == TraceFarField || program == TraceHitLighting || program == TraceCacheRead )
                         block.Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeMissReasonsUB, missReasons,
                                        kW );
#endif
                     block.Texture( ScreenProbeGatherTraceShaderParameters::EnvironmentSpecularTexture,
                                    inputs.EnvSpecular, kS, all )
                          .Sampler( ScreenProbeGatherTraceShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() );
                     if ( lightsHits )
                         block.Texture( ScreenProbeGatherTraceShaderParameters::GBufferDepthTexture,
                                        inputs.GBufferDepth, kS, all )
                              .Sampler( ScreenProbeGatherTraceShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( ScreenProbeGatherTraceShaderParameters::GBufferATexture, inputs.GBufferA,
                                        kS, all )
                              .Texture( ScreenProbeGatherTraceShaderParameters::GBufferEmissiveTexture,
                                        inputs.GBufferEmissive, kS, all );
                     block.Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheIrradianceTexture,
                                    inputs.RadianceCache.Irradiance, kS, all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheDepthTexture,
                                    inputs.RadianceCache.Depth, kS, all )
                          .Texture( ScreenProbeGatherTraceShaderParameters::RadianceCacheRadianceAtlasTexture,
                                    inputs.RadianceCache.Radiance, kS, all )
                          .Storage( ScreenProbeGatherTraceShaderParameters::ScreenProbeTracedTexture, traced, kW );
                 },
                 [pipeline, args = in.Args]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, 0u );
                 } );
        };

        const RayList* const hitLightingOut = hitLightingList ? &*hitLightingList : nullptr;
        // FPS-SP-TRACE-4 (UE compacts the voxel trace's rays to those with distance left): the screen and the near
        // field list a miss at the cache straight for the cache read, so the far field's dispatch holds only rays
        // with distance left to march (a ray at the cache has walked to its tMax already).
        traceWorld( TraceNearField, "ScreenProbes: Trace Near Field", nearList, &farList, &cacheReadList );
        // FPS-NEAR-FIELD (UE TraceMeshSDFs, then its hit lighting pass over the compacted hits): the software near
        // field's hits lit in a list of their own, so the near field's waves wait on no card search, no gradient of
        // an object later passed and no shadow ray of their few hit lanes.
        if ( nearHitLightingList )
            traceWorld( TraceNearHitLighting, "ScreenProbes: Trace Near Hit Lighting", *nearHitLightingList, nullptr,
                        nullptr );
        traceWorld( TraceFarField, "ScreenProbes: Trace Far Field", farList, &cacheReadList, hitLightingOut );
        // GPU-FARFIELD-6: the rays the far field left at the radiance cache's coverage read the cache in a list of
        // their own (UE's trace / shading split), so the far field's waves hold no cache read on their march
        // lanes.
        traceWorld( TraceCacheRead, "ScreenProbes: Trace Cache Read", cacheReadList, &cacheMissList, nullptr );
        // GPU-FARFIELD-3: the far field's global-field hits no card holds, lit in a list of their own (UE's hit
        // lighting is its own compacted pass), so the far field's waves do not wait on their shadow marches.
        if ( hitLightingOut )
            traceWorld( TraceHitLighting, "ScreenProbes: Trace Hit Lighting", *hitLightingOut, nullptr, nullptr );
        // The rays the far field took to the radiance cache's coverage and no traced probe answered (cells the view
        // just reached: allocated this frame past the trace budget, or not yet marked) go on to the full trace
        // distance in a list of their own, so the far field's waves stay as short as its own leg.
        traceWorld( TraceCacheMiss, "ScreenProbes: Trace Cache Miss", cacheMissList, nullptr, nullptr );
#if DESERT_DEV_INSTRUMENTS
        m_MissCounts.AddFrame( graph, m_FrameIndex, missReasons );
#endif

        // HISTORY CLASSIFICATION (UE StochasticLightingTileClassification, PackedPixelData): every surface pixel's
        // history taps reprojected and tested once (ScreenProbeDisocclusion.shader), packed for the temporal
        // filter, and every uniform tile's disoccluded share for the spatial filter. SPATIAL FILTER (UE
        // FilterScreenProbes, SpatialFilterNumPasses): then three passes across neighbouring probes, each reading
        // the last one's radiance.
        RDG::TextureRef radiance = traced;
        if ( inputs.Denoise )
        {
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
                     "ScreenProbes: Spatial Filter", kAsyncPass,
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
                 "ScreenProbes: Filter", kAsyncPass,
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
        // One integrate permutation over its tiles: the simple-diffuse one binds no filtered radiance (it samples
        // no GGX direction).
        const auto integrate = [&]( const Program program, const std::string_view name, const uint64_t argsOffset )
        {
            ComputePipeline* const pipeline = m_Pipelines[program].get();
            const bool             simple   = program == IntegrateSimpleDiffuse;
            graph.AddPass(
                 name, kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[program].Get( m_Shaders[program] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     builder.Read( integrateArgs, RDG::Access::IndirectArgs );
                     // The simple-diffuse program samples nothing linearly (ScreenProbeIntegrate.shader).
                     if ( !simple )
                         block.Texture( IntegrateSlots::ScreenProbeFilteredTexture, filtered, kS, all )
                              .Sampler( ScreenProbeGatherIntegrateShaderParameters::LinearClampSampler,
                                        RDG::SamplerDesc::LinearClamp() );
                     // The args again as storage: the 2D grid's groups past the count read it (a buffer's two
                     // reads merge into one barrier).
                     block.Storage( IntegrateSlots::ScreenProbeIntegrateArgsUB, integrateArgs, kR )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateTilesUB, integrateTileList, kR );
                     block.Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeParamsUB, paramsBuffer,
                                    kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeIndexUB, index, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbesUB, probes, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeSHUB, sh, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeDiffuseOutTexture,
                                    diffuse, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeSpecularOutTexture,
                                    specular, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeBackfaceUB,
                                    backfaceIrradiance, kR )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeBackfaceOutTexture,
                                    backface, kW )
                          .Storage( ScreenProbeGatherIntegrateShaderParameters::ScreenProbeMovingUB, moving, kR )
                          // A pixel no screen probe holds: its tile's mask bit, the tile listed.
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredArgsUB, uncoveredArgs, kW )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredUB, uncoveredTiles, kW )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredMaskUB, uncoveredMask, kW )
                          .Texture( ScreenProbeGatherIntegrateShaderParameters::ShortRangeAoTexture, shortRangeAO,
                                    kS, all )
                          .Sampler( ScreenProbeGatherIntegrateShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() );
                     gbuf( block );
                 },
                 [pipeline, args = integrateArgs, argsOffset]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, argsOffset );
                 } );
        };
        integrate( IntegrateSimpleDiffuse, "ScreenProbes: Integrate Simple Diffuse", 0u );
        integrate( Integrate, "ScreenProbes: Integrate", kIntegrateFullArgsOffset );
        // THE UNCOVERED PIXELS: one group a listed tile, the radiance cache for its masked pixels only.
        {
            ComputePipeline* const pipeline = m_Pipelines[IntegrateUncovered].get();
            graph.AddPass(
                 "ScreenProbes: Integrate Uncovered", kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block =
                          builder.Bindings( m_Layouts[IntegrateUncovered].Get( m_Shaders[IntegrateUncovered] ),
                                            Renderer::GetPipelineRouteFill( *pipeline ) );
                     builder.Read( uncoveredArgs, RDG::Access::IndirectArgs );
                     block.Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredArgsUB, uncoveredArgs, kR )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredUB, uncoveredTiles, kR )
                          .Storage( IntegrateSlots::ScreenProbeIntegrateUncoveredMaskUB, uncoveredMask, kR )
                          .Storage( IntegrateSlots::ScreenProbeParamsUB, paramsBuffer, kR )
                          .Storage( IntegrateSlots::GIFrameUB, inputs.GIFrame, kR )
                          .Sampler( IntegrateSlots::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                          .Storage( IntegrateSlots::ScreenProbeDiffuseOutTexture, diffuse, kW )
                          .Storage( IntegrateSlots::ScreenProbeSpecularOutTexture, specular, kW )
                          .Storage( IntegrateSlots::ScreenProbeBackfaceOutTexture, backface, kW )
                          .Storage( IntegrateSlots::RadianceCacheParamsUB, inputs.RadianceCache.Params, kR )
                          .Storage( IntegrateSlots::RadianceCacheIndirectionUB, inputs.RadianceCache.Indirection,
                                    kR )
                          .Storage( IntegrateSlots::RadianceCacheProbesUB, inputs.RadianceCache.Probes, kR )
                          .Texture( IntegrateSlots::RadianceCacheIrradianceTexture, inputs.RadianceCache.Irradiance,
                                    kS, all )
                          .Texture( IntegrateSlots::RadianceCacheDepthTexture, inputs.RadianceCache.Depth, kS, all )
                          .Texture( IntegrateSlots::ShortRangeAoTexture, shortRangeAO, kS, all )
                          .Sampler( IntegrateSlots::PointClampSampler, RDG::SamplerDesc::PointClamp() );
                     gbuf( block );
                 },
                 [pipeline, args = uncoveredArgs]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, 0u );
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
                 "ScreenProbes: Temporal", kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     using H    = History;
                     auto block = builder.Bindings( m_Layouts[Temporal].Get( m_Shaders[Temporal] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( ScreenProbeGatherTemporalShaderParameters::ScreenProbeParamsUB, paramsBuffer,
                                    kR )
                          .Storage( ScreenProbeGatherTemporalShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Texture( ScreenProbeGatherTemporalShaderParameters::ScreenProbeHistoryTaps, historyTaps,
                                    kS, all )
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

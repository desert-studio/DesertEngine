// SCALABILITY CONTRACT (SCAL1-C0): the interface of the capability catalog, the scalability groups and the
// recommended settings, pinned against the headers before the implementation (SCAL1-S1) was written.
//
// Every device here is a CatalogProbe fixture (an RTX card, an AMD card, MoltenVK on Apple Silicon); no GPU runs.
// The table is a JSON fixture in the shape of Editor/Resources/Config/Scalability.json; one test parses the
// SHIPPED file itself, so a hand edit that breaks the loader's rules reddens here and not at the player's boot.
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/DisplaySettings.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Settings/RecommendedQuality.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/GpuBenchmark.hpp>
#include <Engine/Graphic/Clouds/CloudQuality.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Common::Scalability;
namespace Vk = Desert::Graphic::API::Vulkan;

namespace
{
    // Every group has parameters (real or placeholder), so every group is given all five levels.
    constexpr std::string_view kTable = R"({
  "Version": 1,
  "Groups": {
    "AntiAliasing": {
      "Low":       { "AntiAliasing.Method": "FXAA", "AntiAliasing.Samples": 1, "AntiAliasing.TemporalQuality": 0 },
      "Medium":    { "AntiAliasing.Method": "SMAA", "AntiAliasing.Samples": 1, "AntiAliasing.TemporalQuality": 1 },
      "High":      { "AntiAliasing.Method": "TAA",  "AntiAliasing.Samples": 1, "AntiAliasing.TemporalQuality": 2 },
      "Epic":      { "AntiAliasing.Method": "TAA",  "AntiAliasing.Samples": 1, "AntiAliasing.TemporalQuality": 2 },
      "Cinematic": { "AntiAliasing.Method": "TAA",  "AntiAliasing.Samples": 1, "AntiAliasing.TemporalQuality": 2 }
    },
    "ResolutionScale": {
      "Low":       { "Resolution.Percent": 50,  "Resolution.Upscaler": "TAAU", "Resolution.Sharpness": 20 },
      "Medium":    { "Resolution.Percent": 67,  "Resolution.Upscaler": "TAAU", "Resolution.Sharpness": 20 },
      "High":      { "Resolution.Percent": 100, "Resolution.Upscaler": "None", "Resolution.Sharpness": 0 },
      "Epic":      { "Resolution.Percent": 100, "Resolution.Upscaler": "None", "Resolution.Sharpness": 0 },
      "Cinematic": { "Resolution.Percent": 200, "Resolution.Upscaler": "None", "Resolution.Sharpness": 0 }
    },
    "Filtering": {
      "Low":       { "Filtering.Texture": 1, "Filtering.Anisotropy": 1 },
      "Medium":    { "Filtering.Texture": 2, "Filtering.Anisotropy": 4 },
      "High":      { "Filtering.Texture": 3, "Filtering.Anisotropy": 8 },
      "Epic":      { "Filtering.Texture": 3, "Filtering.Anisotropy": 16 },
      "Cinematic": { "Filtering.Texture": 3, "Filtering.Anisotropy": 16 }
    },
    "ViewDistance": {
      "Low":       { "ViewDistance.MeshLOD": 1, "ViewDistance.FoliageCullPercent": 50, "ViewDistance.FoliageDensityPercent": 50, "ViewDistance.LandscapeLodPercent": 50 },
      "Medium":    { "ViewDistance.MeshLOD": 1, "ViewDistance.FoliageCullPercent": 75, "ViewDistance.FoliageDensityPercent": 75, "ViewDistance.LandscapeLodPercent": 75 },
      "High":      { "ViewDistance.MeshLOD": 1, "ViewDistance.FoliageCullPercent": 100, "ViewDistance.FoliageDensityPercent": 100, "ViewDistance.LandscapeLodPercent": 100 },
      "Epic":      { "ViewDistance.MeshLOD": 1, "ViewDistance.FoliageCullPercent": 100, "ViewDistance.FoliageDensityPercent": 100, "ViewDistance.LandscapeLodPercent": 100 },
      "Cinematic": { "ViewDistance.MeshLOD": 0, "ViewDistance.FoliageCullPercent": 100, "ViewDistance.FoliageDensityPercent": 100, "ViewDistance.LandscapeLodPercent": 100 }
    },
    "Effects": {
      "Low":       { "Effects.CloudLightSamples": 16, "Effects.CloudShadowMapPercent": 50, "Effects.CloudStopTransmittancePerMille": 50, "Effects.ParticleSpawnPercent": 50 },
      "Medium":    { "Effects.CloudLightSamples": 64, "Effects.CloudShadowMapPercent": 50, "Effects.CloudStopTransmittancePerMille": 0, "Effects.ParticleSpawnPercent": 75 },
      "High":      { "Effects.CloudLightSamples": 64, "Effects.CloudShadowMapPercent": 100, "Effects.CloudStopTransmittancePerMille": 0, "Effects.ParticleSpawnPercent": 100 },
      "Epic":      { "Effects.CloudLightSamples": 64, "Effects.CloudShadowMapPercent": 100, "Effects.CloudStopTransmittancePerMille": 0, "Effects.ParticleSpawnPercent": 100 },
      "Cinematic": { "Effects.CloudLightSamples": 64, "Effects.CloudShadowMapPercent": 100, "Effects.CloudStopTransmittancePerMille": 0, "Effects.ParticleSpawnPercent": 100 }
    },
    "Textures": {
      "Low":       { "Textures.MipBias": 100 },
      "Medium":    { "Textures.MipBias": 50 },
      "High":      { "Textures.MipBias": 0 },
      "Epic":      { "Textures.MipBias": 0 },
      "Cinematic": { "Textures.MipBias": 0 }
    },
    "Shadows": {
      "Low":       { "Shadows.Cascades": 2, "Shadows.MapSize": 1024, "Shadows.Distance": 8000,  "Shadows.RayTracing": "None" },
      "Medium":    { "Shadows.Cascades": 3, "Shadows.MapSize": 1024, "Shadows.Distance": 10000, "Shadows.RayTracing": "None" },
      "High":      { "Shadows.Cascades": 4, "Shadows.MapSize": 2048, "Shadows.Distance": 15000, "Shadows.RayTracing": "None" },
      "Epic":      { "Shadows.Cascades": 4, "Shadows.MapSize": 4096, "Shadows.Distance": 20000, "Shadows.RayTracing": "None" },
      "Cinematic": { "Shadows.Cascades": 4, "Shadows.MapSize": 4096, "Shadows.Distance": 30000, "Shadows.RayTracing": "None" }
    },
    "GlobalIllumination": {
      "Low":       { "GlobalIllumination.Samples": 8,  "GlobalIllumination.RayTracing": "None" },
      "Medium":    { "GlobalIllumination.Samples": 16, "GlobalIllumination.RayTracing": "None" },
      "High":      { "GlobalIllumination.Samples": 32, "GlobalIllumination.RayTracing": "None" },
      "Epic":      { "GlobalIllumination.Samples": 48, "GlobalIllumination.RayTracing": "None" },
      "Cinematic": { "GlobalIllumination.Samples": 64, "GlobalIllumination.RayTracing": "None" }
    },
    "Reflections": {
      "Low":       { "Reflections.MaxSteps": 12, "Reflections.RayTracing": "None" },
      "Medium":    { "Reflections.MaxSteps": 20, "Reflections.RayTracing": "None" },
      "High":      { "Reflections.MaxSteps": 32, "Reflections.RayTracing": "None" },
      "Epic":      { "Reflections.MaxSteps": 48, "Reflections.RayTracing": "None" },
      "Cinematic": { "Reflections.MaxSteps": 64, "Reflections.RayTracing": "None" }
    },
    "PostProcess": {
      "Low":       { "PostProcess.AmbientOcclusionSamples": 4,  "PostProcess.BloomMips": 3, "PostProcess.MotionBlurQuality": 0, "PostProcess.DepthOfFieldQuality": 0 },
      "Medium":    { "PostProcess.AmbientOcclusionSamples": 8,  "PostProcess.BloomMips": 4, "PostProcess.MotionBlurQuality": 1, "PostProcess.DepthOfFieldQuality": 1 },
      "High":      { "PostProcess.AmbientOcclusionSamples": 16, "PostProcess.BloomMips": 6, "PostProcess.MotionBlurQuality": 2, "PostProcess.DepthOfFieldQuality": 2 },
      "Epic":      { "PostProcess.AmbientOcclusionSamples": 24, "PostProcess.BloomMips": 6, "PostProcess.MotionBlurQuality": 2, "PostProcess.DepthOfFieldQuality": 2 },
      "Cinematic": { "PostProcess.AmbientOcclusionSamples": 32, "PostProcess.BloomMips": 6, "PostProcess.MotionBlurQuality": 2, "PostProcess.DepthOfFieldQuality": 2 }
    }
  },
  "Recommend": {
    "Thresholds": {
      "AntiAliasing":       [ 20, 60, 120 ],
      "ResolutionScale":    [ 30, 80, 150 ],
      "Filtering":          [ 10, 30, 60 ],
      "ViewDistance":       [ 20, 50, 100 ],
      "Effects":            [ 25, 70, 140 ],
      "Textures":           [ 15, 40, 90 ],
      "Shadows":            [ 25, 70, 140 ],
      "GlobalIllumination": [ 30, 80, 160 ],
      "Reflections":        [ 30, 80, 160 ],
      "PostProcess":        [ 15, 50, 110 ]
    },
    "MinVideoMemoryMiB": {
      "Effects": [ 0, 0, 2048, 4096, 8192 ]
    },
    "DeviceClass": { "Unknown": 10, "Integrated": 15, "AppleUnified": 60, "Discrete": 80 }
  }
})";

    ScalabilityTable Table()
    {
        auto parsed = ScalabilityTable::Parse( kTable );
        EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
        return parsed.GetValue();
    }

    Vk::CatalogProbe RtxProbe()
    {
        Vk::CatalogProbe probe;
        probe.VendorId         = 0x10DE;
        probe.Type             = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        probe.DeviceLocalBytes = 8ull << 30;
        probe.ColorSampleCounts =
             VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
        probe.DepthSampleCounts          = probe.ColorSampleCounts;
        probe.MaxSamplerAnisotropy       = 16.0f;
        probe.MaxImageDimension2D        = 32768;
        probe.TimestampValidBitsGraphics = 64;
        probe.SeparateComputeFamily      = true;
        for ( const Vk::Capability c : { Vk::Capability::RayQuery, Vk::Capability::RayTracingPipeline,
                                         Vk::Capability::AccelerationStructure, Vk::Capability::SamplerAnisotropy,
                                         Vk::Capability::TextureCompressionBC } )
            probe.Caps.Rows[static_cast<std::size_t>( c )].Present = true;
        probe.SurfaceFormats = { { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
                                 { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT } };
        probe.PresentModes   = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                 VK_PRESENT_MODE_IMMEDIATE_KHR };
        return probe;
    }

    Vk::CatalogProbe AmdProbe()
    {
        Vk::CatalogProbe probe                                                                  = RtxProbe();
        probe.VendorId                                                                          = 0x1002;
        probe.Caps.Rows[static_cast<std::size_t>( Vk::Capability::RayTracingPipeline )].Present = false;
        return probe;
    }

    Vk::CatalogProbe AppleSiliconProbe()
    {
        Vk::CatalogProbe probe;
        probe.VendorId                   = 0x106B;
        probe.Type                       = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        probe.DeviceLocalBytes           = 12ull << 30;
        probe.ColorSampleCounts          = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
        probe.DepthSampleCounts          = probe.ColorSampleCounts;
        probe.MaxSamplerAnisotropy       = 16.0f;
        probe.MaxImageDimension2D        = 16384;
        probe.AstcLdrSampled             = true;
        probe.TimestampValidBitsGraphics = 64;
        probe.Portability                = true;
        // MoltenVK may report the RT pipeline extension's feature bit through portability; the builder must
        // still never offer it.
        for ( const Vk::Capability c :
              { Vk::Capability::RayQuery, Vk::Capability::RayTracingPipeline,
                Vk::Capability::AccelerationStructure, Vk::Capability::SamplerAnisotropy,
                Vk::Capability::TextureCompressionBC, Vk::Capability::PortabilitySubset } )
            probe.Caps.Rows[static_cast<std::size_t>( c )].Present = true;
        probe.SurfaceFormats = { { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };
        probe.PresentModes   = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
        return probe;
    }

    QualitySelection AllAt( Level level )
    {
        QualitySelection selection;
        selection.Levels.fill( level );
        return selection;
    }

    bool HasFallback( const ResolvedQuality& resolved, Parameter id )
    {
        return std::any_of( resolved.Fallbacks.begin(), resolved.Fallbacks.end(),
                            [id]( const Fallback& f ) { return f.Id == id; } );
    }
} // namespace

// ---- Parameter specs: one list, complete ------------------------------------------------------------------

TEST( ScalabilityContract, EveryParameterHasOneSpecRowInEnumOrderWithAUniqueKey )
{
    const auto specs = ParameterSpecs();
    ASSERT_EQ( specs.size(), kParameterCount );
    std::set<std::string_view> keys;
    for ( std::size_t i = 0; i < specs.size(); ++i )
    {
        EXPECT_EQ( static_cast<std::size_t>( specs[i].Id ), i );
        EXPECT_LE( specs[i].Min, specs[i].Max ) << specs[i].Key;
        EXPECT_TRUE( keys.insert( specs[i].Key ).second ) << "duplicate key " << specs[i].Key;
        EXPECT_LT( static_cast<std::size_t>( specs[i].Owner ), kGroupCount );
    }
}

TEST( ScalabilityContract, TheRetiredMachineSettingsFieldsAreGroupParameters )
{
    EXPECT_EQ( SpecOf( Parameter::AntiAliasingMethod ).Owner, Group::AntiAliasing );
    EXPECT_EQ( SpecOf( Parameter::AntiAliasingSamples ).Owner, Group::AntiAliasing );
    EXPECT_EQ( SpecOf( Parameter::TextureFilter ).Owner, Group::Filtering );
    EXPECT_EQ( SpecOf( Parameter::Anisotropy ).Owner, Group::Filtering );
    EXPECT_EQ( SpecOf( Parameter::MeshLOD ).Owner, Group::ViewDistance );
    EXPECT_EQ( SpecOf( Parameter::CloudLightSamples ).Owner, Group::Effects );
    EXPECT_EQ( SpecOf( Parameter::CloudShadowMapPercent ).Owner, Group::Effects );
    EXPECT_EQ( SpecOf( Parameter::CloudStopTransmittancePerMille ).Owner, Group::Effects );
    EXPECT_EQ( SpecOf( Parameter::RenderScalePercent ).Owner, Group::ResolutionScale );
    EXPECT_EQ( SpecOf( Parameter::Upscaler ).Owner, Group::ResolutionScale );
}

// ---- The data file ---------------------------------------------------------------------------------------

TEST( ScalabilityContract, TheTableParsesAndAnswersPerLevel )
{
    const ScalabilityTable table = Table();
    EXPECT_EQ( table.Version, 1u );
    EXPECT_EQ( table.ValueAt( Parameter::Anisotropy, Level::Medium ), 4 );
    EXPECT_EQ( table.ValueAt( Parameter::RenderScalePercent, Level::Cinematic ), 200 );
    EXPECT_EQ( table.ValueAt( Parameter::AntiAliasingMethod, Level::High ),
               static_cast<ParameterValue>( AntiAliasingMethod::TAA ) );
}

TEST( ScalabilityContract, TheTableRefusesEveryErrorNotTheFirst )
{
    std::string bad( kTable );
    bad.replace( bad.find( "\"Filtering.Anisotropy\": 4" ), 25, "\"Filtering.Anisotrophy\": 4" ); // unknown key
    // Medium's (Low has 16): out of the 8..64 range.
    bad.replace( bad.find( "\"Effects.CloudLightSamples\": 64" ), 31, "\"Effects.CloudLightSamples\": 99" );
    const auto parsed = ScalabilityTable::Parse( bad );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "Filtering.Anisotrophy" ), std::string::npos );
    EXPECT_NE( parsed.GetError().find( "Effects.CloudLightSamples" ), std::string::npos );
    // Each error names where it sits in the file.
    EXPECT_NE( parsed.GetError().find( "Groups.Effects.Medium" ), std::string::npos ) << parsed.GetError();
}

// No texture streaming pool exists (AM2's per-frame upload budget is not a resident pool), so the
// Textures.StreamingPoolMiB row was deleted rather than kept as a value nothing reads: a table that still
// carries it is refused as an unknown key.
TEST( ScalabilityContract, TheTextureStreamingPoolRowIsGone )
{
    std::string withPool( kTable );
    withPool.replace( withPool.find( "{ \"Textures.MipBias\": 100 }" ), 27,
                      "{ \"Textures.MipBias\": 100, \"Textures.StreamingPoolMiB\": 512 }" );
    const auto parsed = ScalabilityTable::Parse( withPool );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "Textures.StreamingPoolMiB" ), std::string::npos ) << parsed.GetError();
}

// AntiAliasing.Samples is the MSAA count only; TAA's quality is its own (placeholder) row, 0..2.
TEST( ScalabilityContract, SamplesAboveOneUnderANonMsaaMethodIsRefused )
{
    std::string       bad( kTable );
    const std::string high = R"("AntiAliasing.Method": "TAA",  "AntiAliasing.Samples": 1)";
    bad.replace( bad.find( high ), high.size(), R"("AntiAliasing.Method": "TAA",  "AntiAliasing.Samples": 4)" );
    const auto parsed = ScalabilityTable::Parse( bad );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "Groups.AntiAliasing.High" ), std::string::npos ) << parsed.GetError();
    EXPECT_NE( parsed.GetError().find( "MSAA sample count" ), std::string::npos ) << parsed.GetError();
}

TEST( ScalabilityContract, TemporalQualityIsAHiddenPlaceholderOfThreeLevels )
{
    const ParameterSpec& spec = SpecOf( Parameter::TemporalAAQuality );
    EXPECT_EQ( spec.Owner, Group::AntiAliasing );
    EXPECT_EQ( spec.Key, "AntiAliasing.TemporalQuality" );
    EXPECT_EQ( spec.Min, 0 );
    EXPECT_EQ( spec.Max, 2 );
    EXPECT_EQ( spec.NarrowedBy, CatalogList::None );
    EXPECT_TRUE( IsPlaceholder( spec ) );
    std::string       bad( kTable );
    const std::string epic = R"("AntiAliasing.TemporalQuality": 2 },
      "Cinematic")";
    bad.replace( bad.find( epic ), epic.size(), R"("AntiAliasing.TemporalQuality": 3 },
      "Cinematic")" );
    EXPECT_FALSE( ScalabilityTable::Parse( bad ).IsSuccess() );
}

TEST( ScalabilityContract, AParameterUnderAGroupThatDoesNotOwnItIsRefused )
{
    std::string bad( kTable );
    bad.replace( bad.find( "\"ViewDistance.MeshLOD\": 0" ), 25, "\"Effects.CloudLightSamples\": 16" );
    EXPECT_FALSE( ScalabilityTable::Parse( bad ).IsSuccess() );
}

TEST( ScalabilityContract, AGroupMissingItsLevelsIsRefused )
{
    std::string  bad( kTable );
    const size_t from = bad.find( "    \"Shadows\": {" );
    const size_t to   = bad.find( "    \"GlobalIllumination\": {" );
    ASSERT_NE( from, std::string::npos );
    bad.erase( from, to - from );
    const auto parsed = ScalabilityTable::Parse( bad );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "Shadows" ), std::string::npos ) << parsed.GetError();
}

TEST( ScalabilityContract, AnEnumValueIsWrittenByNameNotByNumber )
{
    std::string bad( kTable );
    bad.replace( bad.find( R"("Resolution.Upscaler": "TAAU")" ), 29, R"("Resolution.Upscaler": 1     )" );
    const auto parsed = ScalabilityTable::Parse( bad );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "Resolution.Upscaler" ), std::string::npos ) << parsed.GetError();
}

// ---- Placeholders: reserved rows nobody may see or set ---------------------------------------------------

TEST( ScalabilityContract, EveryPlaceholderIsHiddenAndEveryListedRowNamesAReader )
{
    const std::vector<Parameter> listed = ListedParameters();
    for ( const ParameterSpec& spec : ParameterSpecs() )
    {
        const bool isListed = std::find( listed.begin(), listed.end(), spec.Id ) != listed.end();
        if ( IsPlaceholder( spec ) )
            EXPECT_FALSE( isListed ) << spec.Key << " is a placeholder and must not be listed";
        else
        {
            EXPECT_TRUE( isListed ) << spec.Key;
            const auto& reader = spec.Reader;
            if ( !reader.has_value() )
                FAIL() << spec.Key << " has no reader";
            EXPECT_FALSE( reader->empty() ) << spec.Key << " names no reader";
        }
    }
    // Every group has a real row (Textures since its mip bias gained a reader).
    for ( const Group g :
          { Group::Textures, Group::AntiAliasing, Group::ResolutionScale, Group::Filtering, Group::ViewDistance,
            Group::Effects, Group::Shadows, Group::GlobalIllumination, Group::Reflections, Group::PostProcess } )
        EXPECT_TRUE( IsGroupListed( g ) ) << GroupKey( g );
}

namespace
{
    int              g_SaveCalls = 0;
    QualitySelection g_LastSaved;

    Common::BoolResultStr MockSave( const QualitySelection& selection )
    {
        ++g_SaveCalls;
        g_LastSaved = selection;
        return Common::MakeSuccess( true );
    }

    void InitializeQualityState()
    {
        g_SaveCalls = 0;
        g_LastSaved = {};
        QualityState::Initialize( Table(), Vk::BuildCapabilityCatalog( RtxProbe() ), AllAt( Level::High ),
                                  &MockSave );
    }
} // namespace

TEST( ScalabilityContract, SetOverrideRefusesAPlaceholderAndSavesNothing )
{
    InitializeQualityState();
    const QualitySelection before = QualityState::Selection();
    const auto             report = QualityState::SetOverride( Parameter::ShadowRayTracing, 1 );
    EXPECT_FALSE( report.Refused.empty() );
    EXPECT_FALSE( report.Saved );
    EXPECT_EQ( g_SaveCalls, 0 );
    EXPECT_EQ( QualityState::Selection(), before );
}

TEST( ScalabilityContract, ApplySavesTheSelectionThroughTheSaver )
{
    InitializeQualityState();
    const auto report = QualityState::SetGroupLevel( Group::Shadows, Level::Low );
    EXPECT_TRUE( report.Refused.empty() );
    EXPECT_TRUE( report.Saved );
    EXPECT_EQ( g_SaveCalls, 1 );
    EXPECT_EQ( g_LastSaved.Levels[static_cast<std::size_t>( Group::Shadows )], Level::Low );
    EXPECT_EQ( QualityState::Resolved().As<int32_t>( Parameter::ShadowCascades ), 2 );
}

// ---- Display: VSync is not a quality group ----------------------------------------------------------------

TEST( ScalabilityContract, VSyncOnIsFifoAndOffTakesTheLowestLatencyModeOffered )
{
    const CapabilityCatalog rtx = Vk::BuildCapabilityCatalog( RtxProbe() );
    EXPECT_EQ( ResolvePresentMode( { .VSync = true }, rtx ).Mode, PresentMode::Fifo );
    EXPECT_EQ( ResolvePresentMode( { .VSync = false }, rtx ).Mode, PresentMode::Immediate );
    EXPECT_TRUE( ResolvePresentMode( { .VSync = false }, rtx ).Reason.empty() );

    Vk::CatalogProbe mailboxOnly = RtxProbe();
    mailboxOnly.PresentModes     = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    EXPECT_EQ( ResolvePresentMode( { .VSync = false }, Vk::BuildCapabilityCatalog( mailboxOnly ) ).Mode,
               PresentMode::Mailbox );

    Vk::CatalogProbe fifoOnly = RtxProbe();
    fifoOnly.PresentModes     = { VK_PRESENT_MODE_FIFO_KHR };
    const auto forced         = ResolvePresentMode( { .VSync = false }, Vk::BuildCapabilityCatalog( fifoOnly ) );
    EXPECT_EQ( forced.Mode, PresentMode::Fifo );
    EXPECT_FALSE( forced.Reason.empty() );
}

// ---- The catalog, per device -----------------------------------------------------------------------------

// The fixtures carry only what the real probe produces today: no third-party upscaler SDK is integrated, so every
// DLSS / FSR / XeSS / MetalFX probe flag is false and the catalog offers the engine's own methods only.
TEST( ScalabilityContract, RtxOffersTheRayTracingPipelineAndNoThirdPartyUpscaler )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    EXPECT_EQ( c.Upscalers, ( std::vector<Upscaler>{ Upscaler::TAAU, Upscaler::Spatial, Upscaler::None } ) );
    EXPECT_FALSE( CapabilityCatalog::Offers( c.AntiAliasingMethods, AntiAliasingMethod::DLAA ) );
    EXPECT_TRUE( CapabilityCatalog::Offers( c.RayTracingModes, RayTracingMode::RayTracingPipeline ) );
    EXPECT_EQ( c.MSAACounts, ( std::vector<int>{ 1, 2, 4, 8 } ) );
    EXPECT_EQ( c.AnisotropyLevels, ( std::vector<int>{ 1, 2, 4, 8, 16 } ) );
    EXPECT_TRUE( CapabilityCatalog::Offers( c.DisplayOutputs, DisplayOutput::HDR10_PQ ) );
    EXPECT_EQ( c.TextureCompression, ( std::vector<TextureCompressionFamily>{ TextureCompressionFamily::BC } ) );
    EXPECT_TRUE( c.AsyncCompute );
    EXPECT_EQ( c.Timing, GpuTiming::AnyStage );
    EXPECT_EQ( c.Class, DeviceClass::Discrete );
}

TEST( ScalabilityContract, EveryDeviceOffersTheEnginesOwnMethodsAndFifo )
{
    for ( const Vk::CatalogProbe& probe : { RtxProbe(), AmdProbe(), AppleSiliconProbe() } )
    {
        const CapabilityCatalog c = Vk::BuildCapabilityCatalog( probe );
        for ( const AntiAliasingMethod m : { AntiAliasingMethod::None, AntiAliasingMethod::FXAA,
                                             AntiAliasingMethod::SMAA, AntiAliasingMethod::TAA } )
            EXPECT_TRUE( CapabilityCatalog::Offers( c.AntiAliasingMethods, m ) );
        EXPECT_TRUE( CapabilityCatalog::Offers( c.Upscalers, Upscaler::None ) );
        EXPECT_TRUE( CapabilityCatalog::Offers( c.Upscalers, Upscaler::TAAU ) );
        EXPECT_TRUE( CapabilityCatalog::Offers( c.RayTracingModes, RayTracingMode::None ) );
        EXPECT_TRUE( CapabilityCatalog::Offers( c.PresentModes, PresentMode::Fifo ) );
        EXPECT_TRUE( CapabilityCatalog::Offers( c.DisplayOutputs, DisplayOutput::SDR_sRGB ) );
        ASSERT_FALSE( c.MSAACounts.empty() );
        EXPECT_EQ( c.MSAACounts.front(), 1 );
        EXPECT_FALSE( c.TextureCompression.empty() );
    }
}

TEST( ScalabilityContract, MoltenVkNeverOffersTheRtPipelineOrAsyncComputeAndPrefersAstc )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( AppleSiliconProbe() );
    EXPECT_FALSE( CapabilityCatalog::Offers( c.RayTracingModes, RayTracingMode::RayTracingPipeline ) );
    EXPECT_TRUE( CapabilityCatalog::Offers( c.RayTracingModes, RayTracingMode::RayQuery ) );
    EXPECT_EQ( c.Upscalers, ( std::vector<Upscaler>{ Upscaler::TAAU, Upscaler::Spatial, Upscaler::None } ) );
    EXPECT_FALSE( CapabilityCatalog::Offers( c.AntiAliasingMethods, AntiAliasingMethod::DLAA ) );
    ASSERT_FALSE( c.TextureCompression.empty() );
    EXPECT_EQ( c.TextureCompression.front(), TextureCompressionFamily::ASTC );
    EXPECT_FALSE( c.AsyncCompute );
    EXPECT_EQ( c.Timing, GpuTiming::EncoderBounds );
    EXPECT_EQ( c.Class, DeviceClass::AppleUnified );
}

// ---- Resolution ------------------------------------------------------------------------------------------

TEST( ScalabilityContract, AGroupLevelGivesTheTableValuesWithNoFallbackOnACapableDevice )
{
    const ResolvedQuality r = Resolve( AllAt( Level::High ), Table(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    EXPECT_TRUE( r.Fallbacks.empty() );
    EXPECT_EQ( r.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), AntiAliasingMethod::TAA );
    EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::Anisotropy )], 8 );
    EXPECT_EQ( r.Scale, ScaleMode::Native );
}

// SCAL-COVER: THE EFFECTS LEVEL REACHES THE CLOUD LAYER. Graphic::CloudQualityFor( ResolvedQuality ) is the one
// function SceneRenderer::SetQuality hands the cloud renderer; this drives it from a resolved selection, so a
// Cloud* row nothing reads, or a CloudQualityFor that drops a knob, goes red here.
TEST( ScalabilityContract, TheEffectsLevelAndEachCloudOverrideChangeWhatTheCloudLayerReads )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );

    const Desert::Graphic::CloudQualityScale epic =
         Desert::Graphic::CloudQualityFor( Resolve( AllAt( Level::Epic ), Table(), c ) );
    EXPECT_EQ( epic.LightMarchSampleCeiling, 64 );
    EXPECT_FLOAT_EQ( epic.ShadowMapScale, 1.0f );
    EXPECT_FLOAT_EQ( epic.StopTransmittanceFloor, 0.0f );

    QualitySelection low                                   = AllAt( Level::Epic );
    low.Levels[static_cast<std::size_t>( Group::Effects )] = Level::Low;
    const Desert::Graphic::CloudQualityScale cheap =
         Desert::Graphic::CloudQualityFor( Resolve( low, Table(), c ) );
    EXPECT_EQ( cheap.LightMarchSampleCeiling, 16 );
    EXPECT_FLOAT_EQ( cheap.ShadowMapScale, 0.5f );
    EXPECT_FLOAT_EQ( cheap.StopTransmittanceFloor, 0.05f );

    // Each knob alone, through an override on top of Epic: only its own field moves.
    const auto withOverride = [&]( Parameter parameter, ParameterValue value )
    {
        QualitySelection s = AllAt( Level::Epic );
        s.Overrides.push_back( { std::string( SpecOf( parameter ).Key ), value } );
        return Desert::Graphic::CloudQualityFor( Resolve( s, Table(), c ) );
    };
    const auto light = withOverride( Parameter::CloudLightSamples, 24 );
    EXPECT_EQ( light.LightMarchSampleCeiling, 24 );
    EXPECT_FLOAT_EQ( light.ShadowMapScale, 1.0f );
    const auto map = withOverride( Parameter::CloudShadowMapPercent, 75 );
    EXPECT_FLOAT_EQ( map.ShadowMapScale, 0.75f );
    EXPECT_EQ( map.LightMarchSampleCeiling, 64 );
    const auto stop = withOverride( Parameter::CloudStopTransmittancePerMille, 20 );
    EXPECT_FLOAT_EQ( stop.StopTransmittanceFloor, 0.02f );
    EXPECT_FLOAT_EQ( stop.ShadowMapScale, 1.0f );
}

// SCAL-COVER: THE VIEWDISTANCE LEVEL REACHES FOLIAGE. Graphic::FoliageQualityFor( ResolvedQuality ) is what
// SceneRenderer::SetQuality hands MeshRenderer::SetFoliageQuality. Red if a foliage row stops reaching it, if the
// levels stop differing, or if High/Epic/Cinematic stop being the authored fields (1 / 1).
TEST( ScalabilityContract, TheViewDistanceLevelChangesWhatTheFoliageLoopReads )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
    {
        const Desert::Graphic::FoliageQualityScale same =
             Desert::Graphic::FoliageQualityFor( Resolve( AllAt( level ), Table(), c ) );
        EXPECT_FLOAT_EQ( same.CullDistanceScale, 1.0f );
        EXPECT_FLOAT_EQ( same.Density, 1.0f );
    }
    QualitySelection low                                        = AllAt( Level::Epic );
    low.Levels[static_cast<std::size_t>( Group::ViewDistance )] = Level::Low;
    const Desert::Graphic::FoliageQualityScale cheap =
         Desert::Graphic::FoliageQualityFor( Resolve( low, Table(), c ) );
    EXPECT_FLOAT_EQ( cheap.CullDistanceScale, 0.5f );
    EXPECT_FLOAT_EQ( cheap.Density, 0.5f );

    QualitySelection s = AllAt( Level::Epic );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::FoliageDensityPercent ).Key ), 30 } );
    const Desert::Graphic::FoliageQualityScale dense =
         Desert::Graphic::FoliageQualityFor( Resolve( s, Table(), c ) );
    EXPECT_FLOAT_EQ( dense.Density, 0.3f );
    EXPECT_FLOAT_EQ( dense.CullDistanceScale, 1.0f );
}

// Effects.ParticleSpawnPercent is the share VFX::SpawnState::Step spawns (VFXEmitterSpawn test pins the Step
// side): High and above keep the authored count, Low halves it.
TEST( ScalabilityContract, TheEffectsLevelChangesTheParticleSpawnShare )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
        EXPECT_EQ( Resolve( AllAt( level ), Table(), c ).As<int>( Parameter::ParticleSpawnPercent ), 100 );
    QualitySelection low                                   = AllAt( Level::Epic );
    low.Levels[static_cast<std::size_t>( Group::Effects )] = Level::Low;
    EXPECT_EQ( Resolve( low, Table(), c ).As<int>( Parameter::ParticleSpawnPercent ), 50 );
    EXPECT_EQ( SpecOf( Parameter::ParticleSpawnPercent ).Owner, Group::Effects );
}

// Textures.MipBias is the level-of-detail bias every engine image's own sampler and every graph read naming it
// carry (GlobalTextureFilterSampler; the RenderGraphPassBindings suite pins that the desc carries it): High+
// unbiased.
TEST( ScalabilityContract, TheTexturesLevelChangesTheSamplerMipBias )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
        EXPECT_EQ( Resolve( AllAt( level ), Table(), c ).As<int>( Parameter::TextureMipBias ), 0 );
    QualitySelection sel                                    = AllAt( Level::Epic );
    sel.Levels[static_cast<std::size_t>( Group::Textures )] = Level::Low;
    EXPECT_EQ( Resolve( sel, Table(), c ).As<int>( Parameter::TextureMipBias ), 100 );
    sel.Levels[static_cast<std::size_t>( Group::Textures )] = Level::Medium;
    EXPECT_EQ( Resolve( sel, Table(), c ).As<int>( Parameter::TextureMipBias ), 50 );
    EXPECT_FALSE( IsPlaceholder( SpecOf( Parameter::TextureMipBias ) ) );
    EXPECT_TRUE( IsGroupListed( Group::Textures ) );
}

// ViewDistance.LandscapeLodPercent is the switch-distance scale TerrainRenderer's TileLod reads (the TerrainBatch
// suite pins LandscapeLodAtDistanceScale): High and above authored, Low half the distances.
TEST( ScalabilityContract, TheViewDistanceLevelChangesTheLandscapeLodDistance )
{
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
        EXPECT_EQ( Resolve( AllAt( level ), Table(), c ).As<int>( Parameter::LandscapeLodDistancePercent ), 100 );
    QualitySelection low                                        = AllAt( Level::Epic );
    low.Levels[static_cast<std::size_t>( Group::ViewDistance )] = Level::Low;
    EXPECT_EQ( Resolve( low, Table(), c ).As<int>( Parameter::LandscapeLodDistancePercent ), 50 );
    EXPECT_EQ( SpecOf( Parameter::LandscapeLodDistancePercent ).Owner, Group::ViewDistance );
}

TEST( ScalabilityContract, AnOverrideSitsOnTopOfItsGroupLevelAndTouchesNothingElse )
{
    QualitySelection s = AllAt( Level::High );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::Anisotropy ).Key ), 16 } );
    const CapabilityCatalog c    = Vk::BuildCapabilityCatalog( RtxProbe() );
    const ResolvedQuality   base = Resolve( AllAt( Level::High ), Table(), c );
    const ResolvedQuality   r    = Resolve( s, Table(), c );
    for ( std::size_t i = 0; i < kParameterCount; ++i )
        EXPECT_EQ( r.Values[i], i == static_cast<std::size_t>( Parameter::Anisotropy ) ? 16 : base.Values[i] );
}

TEST( ScalabilityContract, ASavedValueTheDeviceLacksResolvesWithOneReportedFallback )
{
    // The saved value is DLAA, which no catalog offers since the fixtures went DLSS-free (SCAL1-S2): the
    // table's Cinematic row no longer carries it, so the saved selection does, as a user's override would.
    QualitySelection s = AllAt( Level::Cinematic );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
                             static_cast<ParameterValue>( AntiAliasingMethod::DLAA ) } );
    const ResolvedQuality r = Resolve( s, Table(), Vk::BuildCapabilityCatalog( AmdProbe() ) );
    EXPECT_EQ( r.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), AntiAliasingMethod::TAA );
    const auto count = std::count_if( r.Fallbacks.begin(), r.Fallbacks.end(),
                                      []( const Fallback& f ) { return f.Id == Parameter::AntiAliasingMethod; } );
    ASSERT_EQ( count, 1 );
    EXPECT_FALSE( FormatFallback( r.Fallbacks.front() ).empty() );
}

TEST( ScalabilityContract, MsaaCountWalksDownToTheLargestOfferedCount )
{
    QualitySelection s = AllAt( Level::High );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
                             static_cast<ParameterValue>( AntiAliasingMethod::MSAA ) } );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingSamples ).Key ), 8 } );
    const ResolvedQuality r = Resolve( s, Table(), Vk::BuildCapabilityCatalog( AppleSiliconProbe() ) );
    EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::AntiAliasingSamples )], 4 );
    EXPECT_TRUE( HasFallback( r, Parameter::AntiAliasingSamples ) );
}

TEST( ScalabilityContract, BelowNativeScaleAnUpscalerIsMandatoryAndItAntiAliases )
{
    QualitySelection s = AllAt( Level::High );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::RenderScalePercent ).Key ), 67 } );
    const ResolvedQuality r = Resolve( s, Table(), Vk::BuildCapabilityCatalog( AmdProbe() ) );
    EXPECT_EQ( r.Scale, ScaleMode::Upscale );
    EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::TAAU );
    EXPECT_TRUE( HasFallback( r, Parameter::Upscaler ) );
}

// THE UPSCALER IS CHOSEN IN RESOLVE (TAA1-B 6a, SCAL-SPATIAL1): below 100 % temporal AA upscales with TAAU,
// without temporal AA the spatial upscaler does and the method is kept. Mutation: bringing back the 100 % clamp
// keeps FXAA/None native; dropping the TAAU pick leaves Upscaler None; forcing TAA under Spatial changes the
// method.
namespace
{
    ResolvedQuality ResolveAtHigh( std::optional<AntiAliasingMethod> method, ParameterValue percent )
    {
        QualitySelection s = AllAt( Level::High );
        if ( method )
            s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
                                     static_cast<ParameterValue>( *method ) } );
        s.Overrides.push_back( { std::string( SpecOf( Parameter::RenderScalePercent ).Key ), percent } );
        return Resolve( s, Table(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    }
} // namespace

TEST( ScalabilityContract, TaaAtHalfScaleUpscalesWithTaauAtTheAskedPercent )
{
    const ResolvedQuality r = ResolveAtHigh( AntiAliasingMethod::TAA, 50 );
    EXPECT_EQ( r.Scale, ScaleMode::Upscale );
    EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::RenderScalePercent )], 50 );
    EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::TAAU );
    EXPECT_EQ( r.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), AntiAliasingMethod::TAA );
    EXPECT_FALSE( HasFallback( r, Parameter::RenderScalePercent ) );
}

TEST( ScalabilityContract, WithoutTemporalAaHalfScaleUpscalesSpatiallyAndKeepsTheMethod )
{
    for ( const AntiAliasingMethod method :
          { AntiAliasingMethod::FXAA, AntiAliasingMethod::SMAA, AntiAliasingMethod::None } )
    {
        const ResolvedQuality r = ResolveAtHigh( method, 50 );
        EXPECT_EQ( r.Scale, ScaleMode::Upscale ) << static_cast<int>( method );
        EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::RenderScalePercent )], 50 );
        EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::Spatial );
        EXPECT_EQ( r.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), method );
        EXPECT_FALSE( HasFallback( r, Parameter::RenderScalePercent ) ) << static_cast<int>( method );
        EXPECT_FALSE( HasFallback( r, Parameter::AntiAliasingMethod ) ) << static_cast<int>( method );
    }
    // MSAA cannot render below native: FXAA, and FXAA upscales spatially.
    const ResolvedQuality msaa = ResolveAtHigh( AntiAliasingMethod::MSAA, 50 );
    EXPECT_EQ( msaa.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), AntiAliasingMethod::FXAA );
    EXPECT_EQ( msaa.As<Upscaler>( Parameter::Upscaler ), Upscaler::Spatial );
}

TEST( ScalabilityContract, TaaAtNativeScaleRunsNoUpscaler )
{
    const ResolvedQuality r = ResolveAtHigh( AntiAliasingMethod::TAA, 100 );
    EXPECT_EQ( r.Scale, ScaleMode::Native );
    EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::None );
    EXPECT_TRUE( r.Fallbacks.empty() );
}

TEST( ScalabilityContract, TaaAboveNativeScaleIsSupersampledUnchanged )
{
    const ResolvedQuality r = ResolveAtHigh( AntiAliasingMethod::TAA, 150 );
    EXPECT_EQ( r.Scale, ScaleMode::Supersample );
    EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::RenderScalePercent )], 150 );
    EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::None );
    EXPECT_EQ( r.As<AntiAliasingMethod>( Parameter::AntiAliasingMethod ), AntiAliasingMethod::TAA );
    EXPECT_TRUE( r.Fallbacks.empty() );
}

TEST( ScalabilityContract, AboveNativeScaleIsSupersampledWithoutAnUpscaler )
{
    const ResolvedQuality r =
         Resolve( AllAt( Level::Cinematic ), Table(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    EXPECT_EQ( r.Scale, ScaleMode::Supersample );
    EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::None );
}

TEST( ScalabilityContract, MsaaOnAPathThatCannotMultisampleRunsFxaaAndSaysWhy )
{
    QualitySelection s = AllAt( Level::High );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingMethod ).Key ),
                             static_cast<ParameterValue>( AntiAliasingMethod::MSAA ) } );
    const ResolvedQuality  r        = Resolve( s, Table(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    const PathAntiAliasing forward  = ResolveAntiAliasingForPath( r, true );
    const PathAntiAliasing deferred = ResolveAntiAliasingForPath( r, false );
    s.Overrides.push_back( { std::string( SpecOf( Parameter::AntiAliasingSamples ).Key ), 4 } );
    const ResolvedQuality  r4       = Resolve( s, Table(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    const PathAntiAliasing forward4 = ResolveAntiAliasingForPath( r4, true );
    EXPECT_EQ( forward4.Samples, 4 );
    EXPECT_EQ( forward.Method, AntiAliasingMethod::MSAA );
    EXPECT_EQ( forward.Samples, 1 ); // the method alone keeps the level count (High is TAA, so 1)
    EXPECT_TRUE( forward.Reason.empty() );
    EXPECT_EQ( deferred.Method, AntiAliasingMethod::FXAA );
    EXPECT_EQ( deferred.Samples, 1 );
    EXPECT_FALSE( deferred.Reason.empty() );
}

TEST( ScalabilityContract, AnOverrideWithAnUnknownKeyIsReportedNotApplied )
{
    QualitySelection s = AllAt( Level::High );
    s.Overrides.push_back( { "Shadows.FutureKnob", 3 } );
    const CapabilityCatalog c = Vk::BuildCapabilityCatalog( RtxProbe() );
    EXPECT_EQ( Resolve( s, Table(), c ).Values, Resolve( AllAt( Level::High ), Table(), c ).Values );
}

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( std::filesystem::path( prefix ) /
                                       "Editor/Resources/Config/Scalability.json" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }
} // namespace

// The file the packager ships (GamePackager's Config tree), not the fixture: it parses under every loader rule,
// and its High level is what a capable device runs with no fallback.
TEST( ScalabilityContract, TheShippedTableParsesAndHighNeedsNoFallbackOnACapableDevice )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() )
         << "Editor/Resources/Config/Scalability.json not found above the working directory";
    const std::ifstream in( std::filesystem::path( root ) / "Editor/Resources/Config/Scalability.json",
                            std::ios::binary );
    std::ostringstream  text;
    text << in.rdbuf();
    const auto parsed = ScalabilityTable::Parse( text.str() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const ResolvedQuality r =
         Resolve( AllAt( Level::High ), parsed.GetValue(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
    for ( const Fallback& f : r.Fallbacks )
        ADD_FAILURE() << FormatFallback( f );
    EXPECT_EQ( r.Scale, ScaleMode::Native );
}

// Render scale is a person's choice, not a level's: every shipped level renders at 100 % (no frame change).
TEST( ScalabilityContract, EveryShippedLevelResolvesToNativeScale )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::ifstream in( std::filesystem::path( root ) / "Editor/Resources/Config/Scalability.json",
                            std::ios::binary );
    std::ostringstream  text;
    text << in.rdbuf();
    const auto parsed = ScalabilityTable::Parse( text.str() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    for ( std::size_t l = 0; l < kLevelCount; ++l )
    {
        const auto            level = static_cast<Level>( l );
        const ResolvedQuality r =
             Resolve( AllAt( level ), parsed.GetValue(), Vk::BuildCapabilityCatalog( RtxProbe() ) );
        EXPECT_EQ( r.Values[static_cast<std::size_t>( Parameter::RenderScalePercent )], 100 ) << LevelKey( level );
        EXPECT_EQ( r.Scale, ScaleMode::Native ) << LevelKey( level );
        EXPECT_EQ( r.As<Upscaler>( Parameter::Upscaler ), Upscaler::None ) << LevelKey( level );
    }
}

// HIGH IS TODAY'S FRAME. A machine that never chose a level starts on High, so every High value of the shipped
// table must be the value the renderer used before the table existed - otherwise the switch to scalability
// changes the default picture with nobody asking. Each constant is named after the place it lived:
TEST( ScalabilityContract, TheShippedHighLevelIsTheValueEveryReaderUsedBeforeTheTable )
{
    // MachineSettings before SCAL1 (c830e1a8c^): AAMethod = FXAA; MSAASamples = 4 was read only under MSAA, so
    // FXAA ran at one sample; MeshLOD = true; TextureFilterMode = Trilinear; Anisotropy = 8 (read only under
    // Anisotropic); CloudQualityTier = High, whose numbers (CloudQuality.hpp before SCAL-COVER) were the
    // identities: light-march ceiling 64 = ECS::kCloudLightMarchMaxSamples, shadow map scale 1, stop floor 0.
    constexpr auto           kOldAntiAliasing = static_cast<ParameterValue>( AntiAliasingMethod::FXAA );
    constexpr ParameterValue kOldSamples      = 1;
    constexpr ParameterValue kOldMeshLOD      = 1;
    constexpr auto kOldFilter = static_cast<ParameterValue>( Common::Settings::TextureFilter::Trilinear );
    constexpr ParameterValue kOldAnisotropy = 8;
    constexpr ParameterValue kOldCloudLightSamples = 64;
    constexpr ParameterValue kOldCloudShadowMap    = 100;
    constexpr ParameterValue kOldCloudStop         = 0;
    // Graphic::kSceneShadowQuality (ShadowCascades.hpp): 4 cascades of 2048 over 150 m.
    constexpr ParameterValue kOldShadowCascades = 4;
    constexpr ParameterValue kOldShadowMapSize  = 2048;
    constexpr ParameterValue kOldShadowDistance = 15000; // cm
    // SceneRendererFrameDeferred.cpp before S2: RecordTrace( ..., /*maxSteps*/ 32, ...), SSAO Record( ...,
    // /*samples*/ 16 ); GIResolve.shader `const int SAMPLES = 32`; BloomRenderer::kMaxBloomMips = 6.
    constexpr ParameterValue kOldSSRSteps  = 32;
    constexpr ParameterValue kOldSSAOTaps  = 16;
    constexpr ParameterValue kOldGITaps    = 32;
    constexpr ParameterValue kOldBloomMips = 6;
    constexpr ParameterValue kOldScale     = 100;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::ifstream in( std::filesystem::path( root ) / "Editor/Resources/Config/Scalability.json",
                            std::ios::binary );
    std::ostringstream  text;
    text << in.rdbuf();
    const auto parsed = ScalabilityTable::Parse( text.str() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const ScalabilityTable& table = parsed.GetValue();
    const auto              high  = [&]( Parameter p ) { return table.ValueAt( p, Level::High ); };

    EXPECT_EQ( high( Parameter::AntiAliasingMethod ), kOldAntiAliasing );
    EXPECT_EQ( high( Parameter::AntiAliasingSamples ), kOldSamples );
    EXPECT_EQ( high( Parameter::RenderScalePercent ), kOldScale );
    EXPECT_EQ( high( Parameter::Upscaler ), static_cast<ParameterValue>( Upscaler::None ) );
    EXPECT_EQ( high( Parameter::MeshLOD ), kOldMeshLOD );
    EXPECT_EQ( high( Parameter::TextureFilter ), kOldFilter );
    EXPECT_EQ( high( Parameter::Anisotropy ), kOldAnisotropy );
    // The owner's rule for SCAL-COVER: the cloud look at High AND Epic AND Cinematic is the calibrated one.
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
    {
        EXPECT_EQ( table.ValueAt( Parameter::CloudLightSamples, level ), kOldCloudLightSamples )
             << LevelKey( level );
        EXPECT_EQ( table.ValueAt( Parameter::CloudShadowMapPercent, level ), kOldCloudShadowMap )
             << LevelKey( level );
        EXPECT_EQ( table.ValueAt( Parameter::CloudStopTransmittancePerMille, level ), kOldCloudStop )
             << LevelKey( level );
    }
    EXPECT_EQ( high( Parameter::ShadowCascades ), kOldShadowCascades );
    EXPECT_EQ( high( Parameter::ShadowMapSize ), kOldShadowMapSize );
    EXPECT_EQ( high( Parameter::ShadowDistance ), kOldShadowDistance );
    EXPECT_EQ( high( Parameter::ReflectionMaxSteps ), kOldSSRSteps );
    EXPECT_EQ( high( Parameter::AmbientOcclusionSamples ), kOldSSAOTaps );
    EXPECT_EQ( high( Parameter::GlobalIlluminationSamples ), kOldGITaps );
    EXPECT_EQ( high( Parameter::BloomMips ), kOldBloomMips );
    // VulkanImage SamplerCreateInfo before SCAL-COVER: `.mipLodBias = 0.0f` on every image sampler, at every
    // High+.
    for ( const Level level : { Level::High, Level::Epic, Level::Cinematic } )
        EXPECT_EQ( table.ValueAt( Parameter::TextureMipBias, level ), 0 ) << LevelKey( level );
}

// ---- Presets ---------------------------------------------------------------------------------------------

TEST( ScalabilityContract, APresetDecomposesIntoTheTwoAxes )
{
    for ( const AntiAliasingPreset& preset : AntiAliasingPresets() )
    {
        const auto            overrides = Decompose( preset );
        std::set<std::string> keys;
        for ( const ParameterOverride& o : overrides )
            keys.insert( o.Key );
        EXPECT_EQ( keys.size(), 4u ) << preset.Key;
        EXPECT_TRUE( keys.count( std::string( SpecOf( Parameter::RenderScalePercent ).Key ) ) ) << preset.Key;
        EXPECT_TRUE( keys.count( std::string( SpecOf( Parameter::Upscaler ).Key ) ) ) << preset.Key;
    }
}

TEST( ScalabilityContract, OfferedPresetsResolveWithoutAFallback )
{
    for ( const Vk::CatalogProbe& probe : { RtxProbe(), AmdProbe(), AppleSiliconProbe() } )
    {
        const CapabilityCatalog c       = Vk::BuildCapabilityCatalog( probe );
        const auto              offered = OfferedPresets( c );
        EXPECT_FALSE( offered.empty() );
        for ( const AntiAliasingPreset& preset : offered )
        {
            QualitySelection s      = AllAt( Level::High );
            s.Overrides             = Decompose( preset );
            const ResolvedQuality r = Resolve( s, Table(), c );
            EXPECT_FALSE( HasFallback( r, Parameter::AntiAliasingMethod ) ) << preset.Key;
            EXPECT_FALSE( HasFallback( r, Parameter::Upscaler ) ) << preset.Key;
            EXPECT_FALSE( HasFallback( r, Parameter::RenderScalePercent ) ) << preset.Key;
        }
    }
}

// ---- Recommended settings --------------------------------------------------------------------------------

TEST( ScalabilityContract, AFasterGpuIsNeverRecommendedLowerLevels )
{
    const ScalabilityTable table = Table();
    BenchmarkResult        slow;
    BenchmarkResult        fast;
    slow.Timed = fast.Timed = true;
    slow.Class = fast.Class = DeviceClass::Discrete;
    slow.VideoMemory = fast.VideoMemory = 16ull << 30;
    slow.GpuPerfIndex                   = 25.0f;
    fast.GpuPerfIndex                   = 200.0f;
    const auto a                        = RecommendLevels( slow, table );
    const auto b                        = RecommendLevels( fast, table );
    for ( std::size_t g = 0; g < kGroupCount; ++g )
    {
        EXPECT_LE( a[g], b[g] );
        EXPECT_NE( b[g], Level::Cinematic );
    }
}

TEST( ScalabilityContract, VideoMemoryCapsTheLevelWhateverTheIndex )
{
    BenchmarkResult result;
    result.Timed        = true;
    result.Class        = DeviceClass::Discrete;
    result.GpuPerfIndex = 1000.0f;
    result.VideoMemory  = 1ull << 30; // 1 GiB: below Effects' High requirement in the fixture
    EXPECT_LT( RecommendLevels( result, Table() )[static_cast<std::size_t>( Group::Effects )], Level::High );
}

TEST( ScalabilityContract, AnUntimedDeviceTakesTheDeviceClassFallback )
{
    const ScalabilityTable table = Table();
    BenchmarkResult        untimed;
    untimed.Timed           = false;
    untimed.Class           = DeviceClass::AppleUnified;
    untimed.VideoMemory     = 12ull << 30;
    BenchmarkResult asIndex = untimed;
    asIndex.Timed           = true;
    asIndex.GpuPerfIndex    = DeviceClassPerfIndex( DeviceClass::AppleUnified, untimed.VideoMemory, table );
    EXPECT_GT( asIndex.GpuPerfIndex, 0.0f );
    EXPECT_EQ( RecommendLevels( untimed, table ), RecommendLevels( asIndex, table ) );
}

TEST( ScalabilityContract, TheCacheIsValidOnlyForTheSameDeviceDriverAndTable )
{
    const BenchmarkCacheKey key{ 0x10DE, 0x2482, 0x93C00000u, "NVIDIA GeForce RTX 3070 Ti", 1 };
    RecommendedQuality      cached;
    cached.Key = key;
    EXPECT_FALSE( CacheValid( std::nullopt, key ) );
    EXPECT_TRUE( CacheValid( cached, key ) );
    BenchmarkCacheKey newDriver = key;
    newDriver.DriverVersion += 1;
    EXPECT_FALSE( CacheValid( cached, newDriver ) );
    BenchmarkCacheKey newTable = key;
    newTable.TableVersion      = 2;
    EXPECT_FALSE( CacheValid( cached, newTable ) );
}

TEST( ScalabilityContract, AnIndexOnAThresholdTakesTheLevelAboveItAndJustBelowTheLevelUnder )
{
    // The mapping itself, per group, against the table's own boundaries (no VRAM cap: plenty of memory).
    const ScalabilityTable table = Table();
    for ( std::size_t g = 0; g < kGroupCount; ++g )
    {
        for ( std::size_t i = 0; i < table.RecommendThresholds[g].size(); ++i )
        {
            BenchmarkResult at;
            at.Timed              = true;
            at.Class              = DeviceClass::Discrete;
            at.VideoMemory        = 64ull << 30;
            at.GpuPerfIndex       = table.RecommendThresholds[g][i];
            BenchmarkResult below = at;
            below.GpuPerfIndex    = std::nextafter( at.GpuPerfIndex, 0.0f );
            EXPECT_EQ( RecommendLevels( at, table )[g], static_cast<Level>( i + 1 ) )
                 << "group " << g << " t" << i;
            EXPECT_EQ( RecommendLevels( below, table )[g], static_cast<Level>( i ) ) << "group " << g << " t" << i;
        }
    }
}

TEST( ScalabilityContract, ThePerfIndexIsTheGeometricMeanOfThePassRatiosToTheReference )
{
    const std::vector<BenchmarkReference> reference{ { "Alu", 1000.0 }, { "Bandwidth", 50.0 } };

    // Exactly the reference rates -> 100.
    auto same = GpuPerfIndex( { { "Alu", 2.0, 2000.0 }, { "Bandwidth", 4.0, 200.0 } }, reference );
    ASSERT_TRUE( same.IsSuccess() ) << same.GetError();
    EXPECT_NEAR( same.GetValue(), 100.0f, 1e-3f );

    // Twice the reference on every pass -> 200.
    auto twice = GpuPerfIndex( { { "Alu", 1.0, 2000.0 }, { "Bandwidth", 2.0, 200.0 } }, reference );
    ASSERT_TRUE( twice.IsSuccess() ) << twice.GetError();
    EXPECT_NEAR( twice.GetValue(), 200.0f, 1e-3f );

    // Four times faster on one pass, four times slower on the other -> 100, not the arithmetic 212.5.
    auto mixed = GpuPerfIndex( { { "Alu", 0.5, 2000.0 }, { "Bandwidth", 16.0, 200.0 } }, reference );
    ASSERT_TRUE( mixed.IsSuccess() ) << mixed.GetError();
    EXPECT_NEAR( mixed.GetValue(), 100.0f, 1e-3f );
}

TEST( ScalabilityContract, APassThatIsNotARateIsAnErrorNotAnIndex )
{
    const std::vector<BenchmarkReference> reference{ { "Alu", 1000.0 } };
    EXPECT_FALSE( GpuPerfIndex( {}, reference ).IsSuccess() );
    EXPECT_FALSE( GpuPerfIndex( { { "Fill", 1.0, 10.0 } }, reference ).IsSuccess() ); // no reference
    EXPECT_FALSE( GpuPerfIndex( { { "Alu", 0.0, 10.0 } }, reference ).IsSuccess() );  // no time
    EXPECT_FALSE( GpuPerfIndex( { { "Alu", 1.0, 0.0 } }, reference ).IsSuccess() );   // no work
    EXPECT_FALSE( GpuPerfIndex( { { "Alu", 1.0, 10.0 } }, { { "Alu", 0.0 } } ).IsSuccess() );
}

TEST( ScalabilityContract, TheCacheKeyIsTheCreatedDevicesIdentityAndATableChangeInvalidatesIt )
{
    Desert::Engine::DeviceCapabilities caps;
    caps.VendorId               = 0x10DE;
    caps.DeviceId               = 0x2482;
    caps.DriverVersion          = 0x93C00000u;
    caps.Name                   = "NVIDIA GeForce RTX 3070 Ti";
    const BenchmarkCacheKey key = Desert::Engine::MakeBenchmarkCacheKey( caps, 3 );
    EXPECT_EQ( key, ( BenchmarkCacheKey{ 0x10DE, 0x2482, 0x93C00000u, "NVIDIA GeForce RTX 3070 Ti", 3 } ) );

    RecommendedQuality cached;
    cached.Key = key;
    EXPECT_TRUE( CacheValid( cached, Desert::Engine::MakeBenchmarkCacheKey( caps, 3 ) ) );
    EXPECT_FALSE( CacheValid( cached, Desert::Engine::MakeBenchmarkCacheKey( caps, 4 ) ) );
    Desert::Engine::DeviceCapabilities otherGpu = caps;
    otherGpu.DeviceId                           = 0x2484;
    EXPECT_FALSE( CacheValid( cached, Desert::Engine::MakeBenchmarkCacheKey( otherGpu, 3 ) ) );
}

// UpscalerForScale is the one coupling rule (Resolve step 5 and the renderer's per-view resolution both call it,
// so an editor viewport's Screen Percentage cannot pick an upscaler the game setting would not).
TEST( ScalabilityContract, UpscalerForScaleIsResolvesCouplingRule )
{
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::TAA, 50, Upscaler::None ), Upscaler::TAAU );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::TAA, 99, Upscaler::FSR ), Upscaler::FSR ); // vendor override
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::TAA, 100, Upscaler::TAAU ), Upscaler::None );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::TAA, 150, Upscaler::None ), Upscaler::None );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::FXAA, 50, Upscaler::None ), Upscaler::Spatial );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::MSAA, 50, Upscaler::TAAU ), Upscaler::Spatial );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::None, 50, Upscaler::FSR ), Upscaler::Spatial );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::TAA, 50, Upscaler::Spatial ), Upscaler::TAAU );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::FXAA, 100, Upscaler::Spatial ), Upscaler::None );
    EXPECT_EQ( UpscalerForScale( AntiAliasingMethod::None, 100, Upscaler::None ), Upscaler::None );
    EXPECT_TRUE( IsTemporalAntiAliasing( AntiAliasingMethod::DLAA ) );
    EXPECT_FALSE( IsTemporalAntiAliasing( AntiAliasingMethod::SMAA ) );
}

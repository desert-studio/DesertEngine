#include "QualityBoot.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/GpuBenchmark.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <format>
#include <memory>

namespace Desert::Graphic::QualityBoot
{
    namespace
    {
        using Common::Scalability::Parameter;

        // QualityState's Saver: the selection lands in machine.json and nowhere else.
        Common::BoolResultStr SaveSelection( const Common::Scalability::QualitySelection& selection )
        {
            Common::Settings::MachineSettings::Get().Quality = selection;
            if ( !Common::Settings::MachineSettings::Save() )
                return Common::MakeError<bool>(
                     std::format( "{} was not written", Common::Settings::MachineSettings::File().string() ) );
            return Common::MakeSuccess( true );
        }

        // The sampler cache is global state every view shares, so it follows the RESOLUTION, once per change —
        // not each view's BeginScene. Recreating the samplers on a change is what makes the filter apply live.
        void PushSamplerState( const Common::Scalability::ResolvedQuality& resolved, void* /*user*/ )
        {
            const int  filter        = resolved.As<int>( Parameter::TextureFilter );
            const int  anisotropy    = resolved.As<int>( Parameter::Anisotropy );
            const bool filterChanged = RenderConfig::TextureFilter.exchange( filter ) != filter;
            const bool anisoChanged  = RenderConfig::AnisotropyLevel.exchange( anisotropy ) != anisotropy;
            if ( filterChanged || anisoChanged )
                Renderer::GetInstance().RecreateImageSamplers();
        }

        // UE UGameUserSettings::RunHardwareBenchmark on a first launch: a machine that never chose a quality and
        // holds no recommendation measured on THIS device / driver / table (CacheValid) runs the GPU benchmark
        // once, before the first level, and caches the answer in machine.json (Recommended) — StartFrom then
        // starts the machine on it. A run that fails is logged with its reason and the machine starts on
        // StartFrom's no-recommendation answer (High); nothing is invented in its place.
        void MeasureRecommendation( const Common::Scalability::ScalabilityTable&  table,
                                    const Common::Scalability::BenchmarkCacheKey& device )
        {
            using Common::Settings::MachineSettings;
            const std::shared_ptr<Engine::Device> engineDevice = EngineContext::GetInstance().GetDevice();
            if ( !engineDevice )
            {
                LOG_ERROR( "[Scalability] the GPU benchmark did not run: no device; this machine starts on High" );
                return;
            }
            const std::unique_ptr<Engine::GpuBenchmark> benchmark = Engine::GpuBenchmark::Create();
            const auto                                  measured  = benchmark->Run( *engineDevice );
            if ( !measured )
            {
                LOG_ERROR( "[Scalability] the GPU benchmark did not run: {}; this machine starts on High",
                           measured.GetError() );
                return;
            }
            const Common::Scalability::BenchmarkResult& result = measured.GetValue();
            Common::Scalability::RecommendedQuality     recommended;
            recommended.Key                    = device;
            recommended.GpuPerfIndex           = result.GpuPerfIndex;
            recommended.Timed                  = result.Timed;
            recommended.Levels                 = Common::Scalability::RecommendLevels( result, table );
            MachineSettings::Get().Recommended = recommended;
            LOG_INFO( "[Scalability] GPU benchmark on {}: {} (perf index {:.1f})", device.DeviceName,
                      result.Timed ? "timed" : "not timed here, the device-class index", result.GpuPerfIndex );
            if ( !MachineSettings::Save() )
                LOG_ERROR( "[Scalability] the benchmark's recommendation was not written to {}: the next start "
                           "measures again",
                           MachineSettings::File().string() );
        }
    } // namespace

    Common::BoolResultStr Start( const std::filesystem::path& machineJson, Persistence persistence )
    {
        const std::filesystem::path tableFile = Common::Constants::Path::CONFIG_PATH / "Scalability.json";
        const auto                  text      = Common::Utils::FileSystem::ReadFileContent( tableFile );
        if ( !text )
        {
            LOG_ERROR( "[Scalability] {} could not be read: {}", tableFile.string(), text.GetError() );
            return Common::MakeError<bool>( std::format( "{} could not be read", tableFile.string() ) );
        }
        auto table = Common::Scalability::ScalabilityTable::Parse( text.GetValue() );
        if ( !table )
        {
            LOG_ERROR( "[Scalability] {} is not a valid level table: {}", tableFile.string(), table.GetError() );
            return Common::MakeError<bool>( std::format( "{} is not a valid level table", tableFile.string() ) );
        }

        // Loaded by the Application before the device (ApplicationInfo::MachineSettingsFile): the device already
        // read it, so a second Load here could start the quality from a file the device never saw.
        using Common::Settings::MachineSettings;
        if ( MachineSettings::File() != machineJson )
        {
            LOG_ERROR( "[Scalability] the quality starts from {} but the application loaded '{}' "
                       "(ApplicationInfo::MachineSettingsFile)",
                       machineJson.string(), MachineSettings::File().string() );
            return Common::MakeError<bool>(
                 std::format( "{} was not loaded before the device was created", machineJson.string() ) );
        }
        const MachineSettings& machine = MachineSettings::Get();

        // The present pacing lives in machine.json beside the quality (DisplaySettings.hpp); the window rebuilds
        // its swapchain only when the stored value differs from the one it was created with.
        if ( const auto window = EngineContext::GetInstance().GetWindow() )
            window->SetDisplay( machine.Display );

        const auto&                                  capabilities = EngineContext::GetInstance().GetCapabilities();
        const Common::Scalability::BenchmarkCacheKey device =
             Engine::MakeBenchmarkCacheKey( capabilities, table.GetValue().Version );
        // Only where StartFrom would read the recommendation (no saved selection, or a group whose saved level did
        // not read), and never in an unattended run (its machine.json stays byte-for-byte as it was).
        const bool startsFromRecommendation =
             !machine.Quality.has_value() || MachineSettings::UnreadLevels().any();
        if ( persistence == Persistence::MachineFile && startsFromRecommendation &&
             !Common::Scalability::CacheValid( machine.Recommended, device ) )
            MeasureRecommendation( table.GetValue(), device );
        const MachineSettings::StartingQuality start =
             MachineSettings::StartFrom( machine, MachineSettings::UnreadLevels(), device );
        // Load named each refused level (ERROR); this is the level the group actually starts on.
        for ( std::size_t g = 0; g < Common::Scalability::kGroupCount; ++g )
            if ( MachineSettings::UnreadLevels().test( g ) )
                LOG_INFO( "[Scalability] the {} group's saved level did not read: it starts at {}",
                          Common::Scalability::GroupKey( static_cast<Common::Scalability::Group>( g ) ),
                          Common::Scalability::LevelKey( start.Selection.Levels[g] ) );
        // A first run with a valid recommendation starts on High and then APPLIES the recommendation, so the
        // Saver writes it: from then on machine.json holds a selection and the benchmark never overrides it.
        // A named local, not the conditional inline in the by-value argument: MSVC 19.44 never finishes compiling
        // that call (cl.exe spins until killed).
        Common::Scalability::QualitySelection initial =
             start.FromRecommended ? MachineSettings::HighSelection() : start.Selection;
        Common::Scalability::QualityState::Initialize( table.ExtractValue(), capabilities.Catalog,
                                                       std::move( initial ), &SaveSelection );
        const bool thisProcess = persistence == Persistence::ThisProcess;
        if ( thisProcess )
        {
            Common::Scalability::QualityState::KeepInSession( "unattended run" );
            MachineSettings::KeepInSession( "unattended run" );
        }
        Common::Scalability::QualityState::Subscribe( &PushSamplerState, nullptr );
        // Quality.Levels was read from the former positional list or missed a group of this build: the file is
        // rewritten by group name, with the level each group actually starts on (no legacy form kept on disk).
        if ( MachineSettings::LevelsMigrated() && !start.FromRecommended && !thisProcess )
            if ( const auto saved = SaveSelection( start.Selection ); !saved )
                LOG_ERROR( "[Scalability] the migrated quality levels were not written back: {}",
                           saved.GetError() );
        if ( const auto& recommended = machine.Recommended; start.FromRecommended && recommended.has_value() )
        {
            LOG_INFO(
                 "[Scalability] no quality chosen on this machine yet: applying the benchmark's recommendation "
                 "(perf index {:.1f})",
                 recommended->GpuPerfIndex );
            Common::Scalability::QualityState::ApplyRecommended( start.Selection.Levels );
        }
        PushSamplerState( Common::Scalability::QualityState::Resolved(), nullptr );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::QualityBoot

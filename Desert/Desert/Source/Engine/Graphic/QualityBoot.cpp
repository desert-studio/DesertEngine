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
                return Common::MakeError<bool>( std::format( "{} was not written",
                                                             Common::Settings::MachineSettings::File().string() ) );
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
    } // namespace

    Common::BoolResultStr Start( const std::filesystem::path& machineJson )
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

        using Common::Settings::MachineSettings;
        MachineSettings::Load( machineJson, table.GetValue() );
        const MachineSettings& machine = MachineSettings::Get();

        // The present pacing lives in machine.json beside the quality (DisplaySettings.hpp); the window rebuilds
        // its swapchain only when the stored value differs from the one it was created with.
        if ( const auto window = EngineContext::GetInstance().GetWindow() )
            window->SetDisplay( machine.Display );

        const auto&                            capabilities = EngineContext::GetInstance().GetCapabilities();
        const MachineSettings::StartingQuality start        = MachineSettings::StartFrom(
             machine, Engine::MakeBenchmarkCacheKey( capabilities, table.GetValue().Version ) );
        // A first run with a valid recommendation starts on High and then APPLIES the recommendation, so the
        // Saver writes it: from then on machine.json holds a selection and the benchmark never overrides it.
        Common::Scalability::QualityState::Initialize(
             table.ExtractValue(), capabilities.Catalog,
             start.FromRecommended ? MachineSettings::HighSelection() : start.Selection, &SaveSelection );
        Common::Scalability::QualityState::Subscribe( &PushSamplerState, nullptr );
        if ( start.FromRecommended )
        {
            LOG_INFO(
                 "[Scalability] no quality chosen on this machine yet: applying the benchmark's recommendation "
                 "(perf index {:.1f})",
                 machine.Recommended->GpuPerfIndex );
            Common::Scalability::QualityState::ApplyRecommended( start.Selection.Levels );
        }
        PushSamplerState( Common::Scalability::QualityState::Resolved(), nullptr );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::QualityBoot

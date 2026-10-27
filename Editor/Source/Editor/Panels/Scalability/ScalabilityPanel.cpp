#include "ScalabilityPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/DisplayOutput.hpp>
#include <Common/Settings/RecommendedQuality.hpp>
#include <Common/Settings/Scalability.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/GpuBenchmark.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace SC    = Common::Scalability;

    namespace
    {
        // A combo over `count` labelled values; returns the picked index when the user changed it.
        template <typename LabelOf>
        std::optional<std::size_t> ValueCombo( const char* label, std::size_t selected, std::size_t count,
                                               LabelOf&& labelOf )
        {
            std::optional<std::size_t> picked;
            const std::string          preview = selected < count ? labelOf( selected ) : std::string( "-" );
            if ( ImGui::BeginCombo( label, preview.c_str() ) )
            {
                for ( std::size_t i = 0; i < count; ++i )
                {
                    const bool isSelected = i == selected;
                    if ( ImGui::Selectable( labelOf( i ).c_str(), isSelected ) && !isSelected )
                        picked = i;
                    if ( isSelected )
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return picked;
        }

        template <typename T>
        std::size_t IndexOf( const std::vector<T>& list, const T& value )
        {
            const auto it = std::find( list.begin(), list.end(), value );
            return it == list.end() ? list.size() : static_cast<std::size_t>( it - list.begin() );
        }

        std::string LevelLabel( std::size_t level )
        {
            return std::string( SC::LevelKey( static_cast<SC::Level>( level ) ) );
        }

        // "High" when every listed group is recommended one level, else "Shadows Medium, Textures High, ...".
        std::string RecommendedLabel( const std::array<SC::Level, SC::kGroupCount>& levels )
        {
            std::optional<SC::Level> uniform;
            bool                     isUniform = true;
            for ( std::size_t g = 0; g < SC::kGroupCount; ++g )
            {
                if ( !SC::IsGroupListed( static_cast<SC::Group>( g ) ) )
                    continue;
                if ( uniform && *uniform != levels[g] )
                    isUniform = false;
                uniform = levels[g];
            }
            if ( isUniform && uniform )
                return std::string( SC::LevelKey( *uniform ) );
            std::string text;
            for ( std::size_t g = 0; g < SC::kGroupCount; ++g )
            {
                const auto group = static_cast<SC::Group>( g );
                if ( !SC::IsGroupListed( group ) )
                    continue;
                std::format_to( std::back_inserter( text ), "{}{} {}", text.empty() ? "" : ", ",
                                SC::GroupKey( group ), SC::LevelKey( levels[g] ) );
            }
            return text;
        }

        // Requested -> effective for one parameter, from the resolution's own fallback list.
        void ShowFallback( const SC::ResolvedQuality& resolved, SC::Parameter parameter )
        {
            for ( const SC::Fallback& fallback : resolved.Fallbacks )
                if ( fallback.Id == parameter )
                    ImGui::TextDisabled( "%s", SC::FormatFallback( fallback ).c_str() );
        }

        // The value a person asked for: the override if one is set, else the group level's table value.
        SC::ParameterValue Requested( SC::Parameter parameter )
        {
            const SC::QualitySelection& selection = SC::QualityState::Selection();
            const SC::ParameterSpec&    spec      = SC::ParameterSpecs()[static_cast<std::size_t>( parameter )];
            for ( const SC::ParameterOverride& entry : selection.Overrides )
                if ( entry.Key == spec.Key )
                    return entry.Value;
            return SC::QualityState::Table().ValueAt( parameter,
                                                      selection.Levels[static_cast<std::size_t>( spec.Owner )] );
        }
    } // namespace

    // UE's Scalability / GameUserSettings: what THIS MACHINE can afford. Every control here goes through
    // QualityState (the one apply point, which saves machine.json); nothing reaches a scene file. Group levels
    // first (UE's sg.*), then the per-parameter overrides a person tunes alone. Every list a combo offers is the
    // device's CapabilityCatalog list, and every value that does not run as asked shows the resolver's own
    // "requested -> effective (reason)" line. Placeholder parameters and groups with only placeholders are not
    // shown (IsGroupListed): a control that moves nothing is a dead setting.
    // Starts CLOSED, like the other tools in Window -> Tools (Localization).
    ScalabilityPanel::ScalabilityPanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Scalability", /*showPanel=*/false ), m_Scene( scene )
    {
    }

    void ScalabilityPanel::OnUIRender()
    {
        const SC::QualitySelection&  selection = SC::QualityState::Selection();
        const SC::ResolvedQuality&   resolved  = SC::QualityState::Resolved();
        const SC::CapabilityCatalog& catalog   = SC::QualityState::Catalog();

        if ( Utils::ImGuiUtilities::SectionHeader( "Quality Levels" ) )
        {
            if ( const auto all = ValueCombo( "Overall", SC::kLevelCount, SC::kLevelCount, LevelLabel ) )
                SC::QualityState::SetAllGroups( static_cast<SC::Level>( *all ) );
            Utils::ImGuiUtilities::Tooltip( "Sets every group to one level and drops every override." );

            for ( std::size_t g = 0; g < SC::kGroupCount; ++g )
            {
                const auto group = static_cast<SC::Group>( g );
                if ( !SC::IsGroupListed( group ) )
                    continue;
                const std::string label( SC::GroupKey( group ) );
                if ( const auto level = ValueCombo( label.c_str(), static_cast<std::size_t>( selection.Levels[g] ),
                                                    SC::kLevelCount, LevelLabel ) )
                    SC::QualityState::SetGroupLevel( group, static_cast<SC::Level>( *level ) );
            }

            // THE BENCHMARK'S ANSWER for this device, shown only while it is still valid here (same device,
            // driver and table version — CacheValid). A machine with no saved selection already started on it
            // (QualityBoot, MachineSettings::StartFrom); one with a saved selection is offered it.
            const auto&                 recommended = Common::Settings::MachineSettings::Get().Recommended;
            const SC::BenchmarkCacheKey device      = Engine::MakeBenchmarkCacheKey(
                 EngineContext::GetInstance().GetCapabilities(), SC::QualityState::Table().Version );
            if ( recommended.has_value() && SC::CacheValid( recommended, device ) )
            {
                ImGui::TextUnformatted(
                     std::format( "Recommended: {}", RecommendedLabel( recommended->Levels ) ).c_str() );
                ImGui::SameLine();
                if ( ImGui::Button( "Apply recommended" ) )
                    SC::QualityState::ApplyRecommended( recommended->Levels );
                Utils::ImGuiUtilities::Tooltip( "Sets every group to the level the GPU benchmark recommended for "
                                                "this device and drops every override." );
            }
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Anti-Aliasing" ) )
        {
            // ONE METHOD (AA1, UE's r.AntiAliasingMethod + r.MSAACount) and a sample count shown only under
            // MSAA. The list is the catalog's: every method it offers runs here - FSRNative and DLAA only where
            // the device and the build have the vendor upscaler (Graphic/API/UpscalerBuild.hpp). TAA (TAA1-B) and
            // the vendor methods are the ones that let Resolution.Percent go below 100 %.
            const std::vector<SC::AntiAliasingMethod> methods( catalog.AntiAliasingMethods.begin(),
                                                               catalog.AntiAliasingMethods.end() );

            const auto requestedMethod =
                 static_cast<SC::AntiAliasingMethod>( Requested( SC::Parameter::AntiAliasingMethod ) );
            if ( const auto picked = ValueCombo( "Anti-Aliasing Method", IndexOf( methods, requestedMethod ),
                                                 methods.size(), [&methods]( std::size_t i )
                                                 { return std::string( rfl::enum_to_string( methods[i] ) ); } ) )
                SC::QualityState::SetOverride( SC::Parameter::AntiAliasingMethod,
                                               static_cast<SC::ParameterValue>( methods[*picked] ) );
            Utils::ImGuiUtilities::Tooltip( "FXAA and SMAA filter the finished image; MSAA renders the scene "
                                            "at several samples per pixel (forward scenes only: deferred "
                                            "lighting shades one sample per pixel). TAA accumulates jittered "
                                            "frames and is the method that can upscale a lower render scale. "
                                            "Applies on the next frame." );
            ShowFallback( resolved, SC::Parameter::AntiAliasingMethod );

            // What this scene's path runs: MSAA on a deferred scene runs FXAA (AA2), stated, not hidden.
            const auto scene = m_Scene.lock();
            const bool forwardScene =
                 !scene || Desert::Core::RenderPathSupportsMSAA( scene->GetSettings().RenderingPath );
            const SC::PathAntiAliasing path = SC::ResolveAntiAliasingForPath( resolved, forwardScene );
            if ( !path.Reason.empty() )
                ImGui::TextDisabled( "%s", std::format( "This scene runs {}: {}.",
                                                        rfl::enum_to_string( path.Method ), path.Reason )
                                                .c_str() );

            // UE's sg.AntiAliasingQuality for the temporal method: the history's quality (Low / Medium / High).
            // Shown only when this scene's path runs TAA - under any other method it has no reader.
            if ( path.Method == SC::AntiAliasingMethod::TAA )
            {
                static constexpr std::array<const char*, 3> kTemporalQualities = { "Low", "Medium", "High" };
                const int requestedQuality = Requested( SC::Parameter::TemporalAAQuality );
                if ( const auto picked = ValueCombo(
                          "Temporal AA Quality",
                          requestedQuality >= 0 && requestedQuality < static_cast<int>( kTemporalQualities.size() )
                               ? static_cast<std::size_t>( requestedQuality )
                               : kTemporalQualities.size(),
                          kTemporalQualities.size(),
                          []( std::size_t i ) { return std::string( kTemporalQualities[i] ); } ) )
                    SC::QualityState::SetOverride( SC::Parameter::TemporalAAQuality,
                                                   static_cast<SC::ParameterValue>( *picked ) );
                ShowFallback( resolved, SC::Parameter::TemporalAAQuality );
            }

            if ( resolved.As<SC::AntiAliasingMethod>( SC::Parameter::AntiAliasingMethod ) ==
                 SC::AntiAliasingMethod::MSAA )
            {
                std::vector<int> counts;
                for ( const int count : catalog.MSAACounts )
                    if ( count > 1 )
                        counts.push_back( count );
                const int requestedSamples = Requested( SC::Parameter::AntiAliasingSamples );
                if ( const auto picked =
                          ValueCombo( "Samples", IndexOf( counts, requestedSamples ), counts.size(),
                                      [&counts]( std::size_t i ) { return std::format( "{}x", counts[i] ); } ) )
                    SC::QualityState::SetOverride( SC::Parameter::AntiAliasingSamples, counts[*picked] );
                ShowFallback( resolved, SC::Parameter::AntiAliasingSamples );
            }

            // UE's r.NGX.DLSS.<Mode>Preset: which DLSS network each DLSS mode runs. Shown where this device offers
            // DLSS (DLAA at 100 %, the DLSS upscaler below it) - elsewhere the rows have no reader.
            if ( std::find( catalog.Upscalers.begin(), catalog.Upscalers.end(), SC::Upscaler::DLSS ) !=
                 catalog.Upscalers.end() )
            {
                static constexpr std::array<std::pair<SC::Parameter, const char*>, 6> kPresetRows{ {
                     { SC::Parameter::DlssPresetDLAA, "DLSS Preset: DLAA" },
                     { SC::Parameter::DlssPresetUltraQuality, "DLSS Preset: Ultra Quality" },
                     { SC::Parameter::DlssPresetQuality, "DLSS Preset: Quality" },
                     { SC::Parameter::DlssPresetBalanced, "DLSS Preset: Balanced" },
                     { SC::Parameter::DlssPresetPerformance, "DLSS Preset: Performance" },
                     { SC::Parameter::DlssPresetUltraPerformance, "DLSS Preset: Ultra Performance" },
                } };
                for ( const auto& [parameter, label] : kPresetRows )
                {
                    const int requested = Requested( parameter );
                    if ( const auto picked = ValueCombo(
                              label,
                              requested >= 0 && requested < static_cast<int>( SC::kDlssPresetNames.size() )
                                   ? static_cast<std::size_t>( requested )
                                   : SC::kDlssPresetNames.size(),
                              SC::kDlssPresetNames.size(),
                              []( std::size_t i ) { return std::string( SC::kDlssPresetNames[i] ); } ) )
                        SC::QualityState::SetOverride( parameter, static_cast<SC::ParameterValue>( *picked ) );
                    ShowFallback( resolved, parameter );
                }
                Utils::ImGuiUtilities::Tooltip( "Default is NVIDIA's network for the mode. K: transformer, best "
                                                "quality (default for DLAA, Quality, Balanced); M: default for "
                                                "Performance; L: default for Ultra Performance; J: like K with "
                                                "less ghosting and more flicker. Applies on the next frame." );
            }
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Resolution" ) )
        {
            // UE's r.ScreenPercentage (sg.ResolutionQuality): the scene renders at this percent of the output.
            // The range is the device catalog's. Which upscaler runs is NOT a choice here: Scalability Resolve
            // picks it from the AA method (TAA below 100 % -> TAAU; no temporal AA -> the spatial upscaler).
            // Resolution.Upscaler is a vendor override (FSR / DLSS / MetalFX), offered only when the catalog lists
            // one; Resolution.Sharpness is the post sharpen after the resolve (SceneRenderer, Graphic::Sharpen).
            const int requestedScale =
                 m_DraggedRenderScale.value_or( Requested( SC::Parameter::RenderScalePercent ) );
            int scale = requestedScale;
            if ( ImGui::SliderInt( "Render Scale", &scale, catalog.RenderScale.MinPercent,
                                   catalog.RenderScale.MaxPercent, "%d %%", ImGuiSliderFlags_AlwaysClamp ) )
                m_DraggedRenderScale = scale;
            if ( ImGui::IsItemDeactivatedAfterEdit() && m_DraggedRenderScale.has_value() )
            {
                SC::QualityState::SetOverride( SC::Parameter::RenderScalePercent, *m_DraggedRenderScale );
                m_DraggedRenderScale.reset();
            }
            Utils::ImGuiUtilities::Tooltip( "Below 100 % the scene renders smaller and TAA upscales it to the "
                                            "output (without TAA the spatial upscaler does); above 100 % it is "
                                            "supersampled. Applies on release." );
            ShowFallback( resolved, SC::Parameter::RenderScalePercent );

            std::vector<SC::Upscaler> vendors;
            for ( const SC::Upscaler upscaler : catalog.Upscalers )
                if ( upscaler != SC::Upscaler::None && upscaler != SC::Upscaler::TAAU &&
                     upscaler != SC::Upscaler::TSR && upscaler != SC::Upscaler::Spatial )
                    vendors.push_back( upscaler );
            // The engine's own temporal upscaler (UE: TSR vs TAAU), used below 100 % when no vendor override is
            // picked and when the picked one is not offered.
            const auto engineTemporal =
                 static_cast<SC::EngineTemporalUpscaler>( Requested( SC::Parameter::TemporalUpscaler ) );
            if ( const auto picked = ValueCombo( "Temporal Upscaler", static_cast<std::size_t>( engineTemporal ),
                                                 SC::kEngineTemporalUpscalerNames.size(), []( std::size_t i )
                                                 { return std::string( SC::kEngineTemporalUpscalerNames[i] ); } ) )
                SC::QualityState::SetOverride( SC::Parameter::TemporalUpscaler,
                                               static_cast<SC::ParameterValue>( *picked ) );
            Utils::ImGuiUtilities::Tooltip( "The engine's temporal upscaler below 100 %: TSR (temporal super "
                                            "resolution, UE TSR) or TAAU. A vendor override replaces it where "
                                            "the device offers one." );
            ShowFallback( resolved, SC::Parameter::TemporalUpscaler );
            if ( resolved.As<SC::EngineTemporalUpscaler>( SC::Parameter::TemporalUpscaler ) ==
                 SC::EngineTemporalUpscaler::TSR )
            {
                // UE's sg.AntiAliasingQuality rows of r.TSR.*; the level sets them, an override replaces one.
                const auto tsrSlider = [&]( const char* label, SC::Parameter parameter, const char* tooltip )
                {
                    const SC::ParameterSpec& spec  = SC::SpecOf( parameter );
                    int                      value = Requested( parameter );
                    if ( ImGui::SliderInt( label, &value, spec.Min, spec.Max, "%d",
                                           ImGuiSliderFlags_AlwaysClamp ) )
                        SC::QualityState::SetOverride( parameter, static_cast<SC::ParameterValue>( value ) );
                    Utils::ImGuiUtilities::Tooltip( tooltip );
                    ShowFallback( resolved, parameter );
                };
                tsrSlider( "TSR History %", SC::Parameter::TsrHistoryScreenPercentage,
                           "r.TSR.History.ScreenPercentage: the history's size in percent of the output." );
                tsrSlider( "TSR Update Quality", SC::Parameter::TsrHistoryUpdateQuality,
                           "r.TSR.History.UpdateQuality: how the history is resampled and clamped (0..3)." );
                tsrSlider( "TSR Flickering", SC::Parameter::TsrShadingRejectionFlickering,
                           "r.TSR.ShadingRejection.Flickering: keep the history of flickering detail (1) or "
                           "reject it like any other change (0)." );
                tsrSlider(
                     "TSR Rejection AA", SC::Parameter::TsrRejectionAntiAliasing,
                     "r.TSR.RejectionAntiAliasingQuality: spatial anti-aliasing of rejected pixels (0 off)." );
                tsrSlider( "TSR Reprojection Field", SC::Parameter::TsrReprojectionField,
                           "r.TSR.ReprojectionField: reproject with the depth-anti-aliased reprojection field and its "
                           "jacobian (1) or the dilated velocity alone (0)." );
                tsrSlider( "TSR Resurrection", SC::Parameter::TsrResurrection,
                           "r.TSR.Resurrection: keep persistent frames and resurrect disoccluded history from them." );
            }
            if ( !vendors.empty() )
            {
                const auto requestedUpscaler = static_cast<SC::Upscaler>( Requested( SC::Parameter::Upscaler ) );
                std::vector<SC::Upscaler> choices{ SC::Upscaler::None };
                choices.insert( choices.end(), vendors.begin(), vendors.end() );
                if ( const auto picked =
                          ValueCombo( "Upscaler Override", IndexOf( choices, requestedUpscaler ), choices.size(),
                                      [&choices]( std::size_t i ) {
                                          return i == 0 ? std::string( "Engine" )
                                                        : std::string( rfl::enum_to_string( choices[i] ) );
                                      } ) )
                    SC::QualityState::SetOverride( SC::Parameter::Upscaler,
                                                   static_cast<SC::ParameterValue>( choices[*picked] ) );
            }
            ImGui::TextDisabled( "%s", std::format( "Upscaler: {}", rfl::enum_to_string( resolved.As<SC::Upscaler>(
                                                                         SC::Parameter::Upscaler ) ) )
                                            .c_str() );
            ShowFallback( resolved, SC::Parameter::Upscaler );

            int sharpness = Requested( SC::Parameter::UpscalerSharpness );
            if ( ImGui::SliderInt( "Sharpness", &sharpness, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp ) )
                SC::QualityState::SetOverride( SC::Parameter::UpscalerSharpness,
                                               static_cast<SC::ParameterValue>( sharpness ) );
            Utils::ImGuiUtilities::Tooltip(
                 "Post sharpen (RCAS) after TAA, TAAU or the spatial upscale; 0 is off. "
                 "Native frames without TAA are not sharpened." );
            ShowFallback( resolved, SC::Parameter::UpscalerSharpness );
        }

        // THE MACHINE'S CEILING of each render method (Common/Settings/RenderMethods.hpp). A view's own method
        // (its PostProcessVolume) above the ceiling is lowered by Graphic::ResolveRenderMethods, which also applies
        // the device; the viewport overlay shows what each view really renders.
        if ( Utils::ImGuiUtilities::SectionHeader( "Render Methods" ) )
        {
            const auto methodCombo = [&]( const char* label, SC::Parameter parameter,
                                          std::span<const std::string_view> names )
            {
                const auto requested = static_cast<std::size_t>( Requested( parameter ) );
                if ( const auto picked = ValueCombo( label, requested, names.size(),
                                                     [names]( std::size_t i ) { return std::string( names[i] ); } ) )
                    SC::QualityState::SetOverride( parameter, static_cast<SC::ParameterValue>( *picked ) );
            };
            methodCombo( "Global Illumination", SC::Parameter::GlobalIlluminationMethod, SC::kGIMethodNames );
            methodCombo( "Reflections", SC::Parameter::ReflectionsMethod, SC::kReflectionMethodNames );
            methodCombo( "Shadows", SC::Parameter::ShadowsMethod, SC::kShadowMethodNames );
            ImGui::TextDisabled( "Ceilings: a view never renders above them. Traced methods also need\n"
                                 "ray query on the device; unimplemented methods are skipped downwards." );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Textures" ) )
        {
            // Global sampler filter — applies live (QualityBoot's listener recreates the samplers).
            static constexpr const char* kFilters[] = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
            const auto filter = static_cast<std::size_t>( Requested( SC::Parameter::TextureFilter ) );
            if ( const auto picked = ValueCombo( "Filter", filter, std::size( kFilters ),
                                                 []( std::size_t i ) { return std::string( kFilters[i] ); } ) )
                SC::QualityState::SetOverride( SC::Parameter::TextureFilter,
                                               static_cast<SC::ParameterValue>( *picked ) );

            // Anisotropy moves nothing outside the Anisotropic filter, so it is shown only there.
            if ( resolved.As<int>( SC::Parameter::TextureFilter ) == 3 )
            {
                const std::vector<int>& levels = catalog.AnisotropyLevels;
                if ( const auto picked = ValueCombo(
                          "Anisotropy", IndexOf( levels, Requested( SC::Parameter::Anisotropy ) ), levels.size(),
                          [&levels]( std::size_t i ) { return std::format( "{}x", levels[i] ); } ) )
                    SC::QualityState::SetOverride( SC::Parameter::Anisotropy, levels[*picked] );
                ShowFallback( resolved, SC::Parameter::Anisotropy );
            }
        }

        // HDR-OUTPUT: the display output (machine.json Display, beside VSync). Only the catalog's DisplayOutputs
        // are offered; the swapchain rebuilds with the pick (Window::SetDisplay) and logs what it built.
        if ( Utils::ImGuiUtilities::SectionHeader( "Display" ) )
        {
            auto&       machine = Common::Settings::MachineSettings::Get();
            const auto& outputs = EngineContext::GetInstance().GetCapabilities().Catalog.DisplayOutputs;
            bool        changed = false;
            if ( const auto picked = ValueCombo(
                      "Display Output", IndexOf( outputs, machine.Display.Output ), outputs.size(),
                      [&]( std::size_t i ) { return std::string( SC::DisplayOutputName( outputs[i] ) ); } ) )
            {
                machine.Display.Output = outputs[*picked];
                changed                = true;
            }
            changed |= ImGui::DragFloat( "Paper White (nits)", &machine.Display.PaperWhiteNits, 1.0f, 1.0f, 1000.0f,
                                         "%.0f" );
            changed |= ImGui::DragFloat( "Peak Override (nits, 0 = OS)", &machine.Display.PeakNits, 10.0f, 0.0f,
                                         10000.0f, "%.0f" );
            if ( changed )
            {
                (void)Common::Settings::MachineSettings::Save();
                EngineContext::GetInstance().GetWindow()->SetDisplay( machine.Display );
            }
            const Graphic::DisplayOutputState& state = Graphic::CurrentDisplayOutput();
            ImGui::TextDisabled( "Presenting %s; paper white %.0f nits, peak %.0f nits, headroom %.2f",
                                 std::string( SC::DisplayOutputName( state.Output ) ).c_str(), state.PaperWhiteNits,
                                 state.PeakNits, state.Headroom );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Clouds" ) )
        {
            static constexpr const char* kCloud[] = { "Low", "Medium", "High" };
            const auto cloud = static_cast<std::size_t>( Requested( SC::Parameter::CloudQuality ) );
            if ( const auto picked = ValueCombo( "Cloud Quality", cloud, std::size( kCloud ),
                                                 []( std::size_t i ) { return std::string( kCloud[i] ); } ) )
                SC::QualityState::SetOverride( SC::Parameter::CloudQuality,
                                               static_cast<SC::ParameterValue>( *picked ) );
            ImGui::TextDisabled( "High is the calibrated reference. Medium halves the cloud shadow map's\n"
                                 "reach on the ground (~15 km); Low also caps the sun-ray at 16 samples,\n"
                                 "which runs the sunward highlights bright." );
        }
    }
} // namespace Desert::Editor

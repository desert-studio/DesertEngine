#include "ScalabilityPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>
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
            // MSAA. The list is the catalog's, minus the vendor temporal methods (FSRNative / DLAA): no pass runs
            // them in this build, so offering them would be a dead setting. TAA runs (TAA1-B) and is the method
            // that lets Resolution.Percent go below 100 %.
            std::vector<SC::AntiAliasingMethod> methods;
            for ( const SC::AntiAliasingMethod method : catalog.AntiAliasingMethods )
                if ( method == SC::AntiAliasingMethod::None || method == SC::AntiAliasingMethod::FXAA ||
                     method == SC::AntiAliasingMethod::SMAA || method == SC::AntiAliasingMethod::MSAA ||
                     method == SC::AntiAliasingMethod::TAA )
                    methods.push_back( method );

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
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Resolution" ) )
        {
            // UE's r.ScreenPercentage (sg.ResolutionQuality): the scene renders at this percent of the output.
            // The range is the device catalog's. Which upscaler runs is NOT a choice here: Scalability Resolve
            // picks it from the AA method (TAA below 100 % -> TAAU; no temporal AA -> stays 100 %, reported
            // below). Resolution.Upscaler is a vendor override (FSR / DLSS / MetalFX), offered only when the
            // catalog lists one; Resolution.Sharpness has no reader in this build and is not shown.
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
                                            "output (needs TAA); above 100 % it is supersampled. Applies on "
                                            "release." );
            ShowFallback( resolved, SC::Parameter::RenderScalePercent );

            std::vector<SC::Upscaler> vendors;
            for ( const SC::Upscaler upscaler : catalog.Upscalers )
                if ( upscaler != SC::Upscaler::None && upscaler != SC::Upscaler::TAAU )
                    vendors.push_back( upscaler );
            if ( !vendors.empty() )
            {
                const auto requestedUpscaler = static_cast<SC::Upscaler>( Requested( SC::Parameter::Upscaler ) );
                std::vector<SC::Upscaler> choices{ SC::Upscaler::None };
                choices.insert( choices.end(), vendors.begin(), vendors.end() );
                if ( const auto picked =
                          ValueCombo( "Upscaler Override", IndexOf( choices, requestedUpscaler ), choices.size(),
                                      [&choices]( std::size_t i ) {
                                          return i == 0 ? std::string( "Engine (TAAU)" )
                                                        : std::string( rfl::enum_to_string( choices[i] ) );
                                      } ) )
                    SC::QualityState::SetOverride( SC::Parameter::Upscaler,
                                                   static_cast<SC::ParameterValue>( choices[*picked] ) );
            }
            ImGui::TextDisabled( "%s", std::format( "Upscaler: {}", rfl::enum_to_string( resolved.As<SC::Upscaler>(
                                                                         SC::Parameter::Upscaler ) ) )
                                            .c_str() );
            ShowFallback( resolved, SC::Parameter::Upscaler );
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

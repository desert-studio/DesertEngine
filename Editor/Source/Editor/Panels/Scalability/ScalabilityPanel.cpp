#include "ScalabilityPanel.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/DisplayOutput.hpp>
#include <Editor/Panels/Scalability/DisplayOutputPaletteCommands.hpp>
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

        // The listed groups (IsGroupListed): the ones a level slider moves.
        std::vector<SC::Group> ListedGroups()
        {
            std::vector<SC::Group> groups;
            for ( std::size_t g = 0; g < SC::kGroupCount; ++g )
                if ( SC::IsGroupListed( static_cast<SC::Group>( g ) ) )
                    groups.push_back( static_cast<SC::Group>( g ) );
            return groups;
        }

        // What a person reads for a group (GroupKey is the file's and machine.json's name).
        const char* GroupLabel( SC::Group group )
        {
            switch ( group )
            {
                case SC::Group::Textures:
                    return "Textures";
                case SC::Group::Filtering:
                    return "Texture Filtering";
                case SC::Group::Shadows:
                    return "Shadows";
                case SC::Group::GlobalIllumination:
                    return "Global Illumination";
                case SC::Group::Reflections:
                    return "Reflections";
                case SC::Group::PostProcess:
                    return "Post Processing";
                case SC::Group::Effects:
                    return "Effects";
                case SC::Group::ViewDistance:
                    return "View Distance";
                case SC::Group::AntiAliasing:
                    return "Anti-Aliasing";
                case SC::Group::ResolutionScale:
                    return "Resolution Scale";
                case SC::Group::Shading:
                    return "Shading";
                case SC::Group::PathTracing:
                    return "Path Tracing";
                case SC::Group::Foliage:
                    return "Foliage";
                case SC::Group::Count:
                    break;
            }
            return "?";
        }

        // A level slider (UE's Scalability panel): Low .. Cinematic, the level's name as its text; "Custom" when
        // @p current is empty (the Overall slider over groups at different levels). Returns the new level when
        // moved.
        std::optional<SC::Level> LevelSlider( const char* label, std::optional<SC::Level> current )
        {
            int               value = static_cast<int>( current.value_or( SC::Level::Epic ) );
            const std::string text  = current ? std::string( SC::LevelKey( *current ) ) : std::string( "Custom" );
            if ( !ImGui::SliderInt( label, &value, 0, static_cast<int>( SC::kLevelCount ) - 1, text.c_str(),
                                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput ) )
                return std::nullopt;
            const auto level = static_cast<SC::Level>( value );
            if ( current == level )
                return std::nullopt;
            return level;
        }

        // The level every listed group is at, or empty when they differ.
        std::optional<SC::Level> UniformLevel( const SC::GroupLevels& levels )
        {
            std::optional<SC::Level> uniform;
            for ( const SC::Group group : ListedGroups() )
            {
                const SC::Level level = levels[static_cast<std::size_t>( group )];
                if ( uniform && *uniform != level )
                    return std::nullopt;
                uniform = level;
            }
            return uniform;
        }
    } // namespace

    // UE's Scalability / GameUserSettings: what THIS MACHINE can afford. Every control here goes through
    // QualityState (the one apply point, which saves machine.json); nothing reaches a scene file.
    // SCAL-UX (owner 10-27, "too many switches"): what a person sees is UE's shape - an Overall slider and one
    // level slider per listed group. The per-parameter overrides are folded away under "Advanced" (closed): first
    // the Basic parameters with their curated controls, then every Advanced one, drawn from its spec, under a
    // closed "Internal parameters" tree. Every list a combo offers is the device's CapabilityCatalog list, and
    // every value that does not run as asked shows the resolver's own "requested -> effective (reason)" line.
    // Placeholder parameters and groups with only placeholders are not shown (IsGroupListed): a control that
    // moves nothing is a dead setting.
    // Starts CLOSED, like the other tools in Window -> Tools (Localization).
    ScalabilityPanel::ScalabilityPanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Scalability", /*showPanel=*/false ), m_Scene( scene )
    {
    }

    void ScalabilityPanel::OnUIRender()
    {
        const SC::QualitySelection& selection = SC::QualityState::Selection();

        if ( Utils::ImGuiUtilities::SectionHeader( "Quality Levels" ) )
        {
            ImGui::PushID( "Levels" );
            if ( const auto all = LevelSlider( "Overall", UniformLevel( selection.Levels ) ) )
                SC::QualityState::SetAllGroups( *all );
            Utils::ImGuiUtilities::Tooltip( "Sets every group to one level and drops every override." );

            for ( const SC::Group group : ListedGroups() )
                if ( const auto level =
                          LevelSlider( GroupLabel( group ), selection.Levels[static_cast<std::size_t>( group )] ) )
                    SC::QualityState::SetGroupLevel( group, *level );
            ImGui::PopID();

            // THE BENCHMARK'S ANSWER for this device, shown only while it is still valid here (same device,
            // driver and table version - CacheValid). A machine with no saved selection already started on it
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
            if ( !selection.Overrides.empty() )
                ImGui::TextDisabled( "%s", std::format( "{} parameter override(s) on top of the levels (Advanced).",
                                                        selection.Overrides.size() )
                                                .c_str() );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Advanced", /*defaultOpen=*/false ) )
        {
            ImGui::PushID( "Advanced" );
            ImGui::Indent();
            DrawBasicParameters();
            DrawInternalParameters();
            ImGui::Unindent();
            ImGui::PopID();
        }
    }

    void ScalabilityPanel::DrawBasicParameters()
    {
        const SC::ResolvedQuality&   resolved = SC::QualityState::Resolved();
        const SC::CapabilityCatalog& catalog  = SC::QualityState::Catalog();

        if ( Utils::ImGuiUtilities::SectionHeader( "Anti-Aliasing" ) )
        {
            // ONE METHOD (AA1, UE's r.AntiAliasingMethod + r.MSAACount) and a sample count shown only under
            // MSAA. The list is the catalog's: every method it offers runs here - FSR / DLSS / DLSS Ray
            // Reconstruction only where the device and the build have the vendor SDK (Graphic/API/
            // UpscalerBuild.hpp). A temporal method (TAA, TSR, the vendor ones) runs at any Resolution.Percent
            // <= 100 and reconstructs the output itself; the percent is the other axis, below.
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
        }

        if ( Utils::ImGuiUtilities::SectionHeader( "Resolution" ) )
        {
            // UE's r.ScreenPercentage (sg.ResolutionQuality): the scene renders at this percent of the output.
            // The range is the device catalog's. What reconstructs the output is NOT a choice here: it is the AA
            // method's own pass (Scalability UpscalerForScale: TAA below 100 % -> TAAU, TSR / DLSS / FSR their
            // own; no temporal AA -> the spatial upscaler). Resolution.Sharpness is the post sharpen after the
            // resolve (SceneRenderer, Graphic::Sharpen).
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
            Utils::ImGuiUtilities::Tooltip( "Below 100 % the scene renders smaller and the temporal AA method "
                                            "reconstructs the output (without one the spatial upscaler does); "
                                            "above 100 % it is supersampled and only TAA runs. Applies on "
                                            "release." );
            ShowFallback( resolved, SC::Parameter::RenderScalePercent );

            const auto resolvedMethod = resolved.As<SC::AntiAliasingMethod>( SC::Parameter::AntiAliasingMethod );
            const SC::Upscaler reconstruction =
                 SC::UpscalerForScale( resolvedMethod, resolved.As<int>( SC::Parameter::RenderScalePercent ) );
            ImGui::TextDisabled( "%s",
                                 std::format( "Reconstruction: {}",
                                              SC::kUpscalerNames[static_cast<std::size_t>( reconstruction )] )
                                      .c_str() );

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
                (void)ApplyMachineDisplay();
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

        if ( Utils::ImGuiUtilities::SectionHeader( "Path Tracing" ) )
        {
            // The path tracer's budget (Group::PathTracing): read only while a view runs GI / reflections
            // PathTraced.
            for ( const SC::Parameter parameter : SC::ListedParameters( SC::ParameterVisibility::Basic ) )
                if ( SC::SpecOf( parameter ).Owner == SC::Group::PathTracing )
                    DrawParameterRow( parameter );
        }
    }

    void ScalabilityPanel::DrawInternalParameters()
    {
        // UE's r.* under sg.*: the level sets every one of them; a row here overrides one alone.
        if ( !ImGui::TreeNodeEx( "Internal parameters", ImGuiTreeNodeFlags_SpanAvailWidth ) )
            return;
        const std::vector<SC::Parameter> advanced = SC::ListedParameters( SC::ParameterVisibility::Advanced );
        for ( const SC::Group group : ListedGroups() )
        {
            const bool any = std::any_of( advanced.begin(), advanced.end(), [group]( SC::Parameter parameter )
                                          { return SC::SpecOf( parameter ).Owner == group; } );
            if ( !any || !ImGui::TreeNodeEx( GroupLabel( group ), ImGuiTreeNodeFlags_SpanAvailWidth ) )
                continue;
            for ( const SC::Parameter parameter : advanced )
                if ( SC::SpecOf( parameter ).Owner == group )
                    DrawParameterRow( parameter );
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    void ScalabilityPanel::DrawParameterRow( const SC::Parameter parameter )
    {
        const SC::ParameterSpec&   spec     = SC::SpecOf( parameter );
        const SC::ResolvedQuality& resolved = SC::QualityState::Resolved();
        const std::string          label( spec.Key );
        ImGui::PushID( label.c_str() );

        const std::span<const std::string_view> names     = SC::ParameterValueNames( parameter );
        const SC::ParameterValue                requested = Requested( parameter );
        if ( !names.empty() )
        {
            const std::size_t selected = requested >= 0 && static_cast<std::size_t>( requested ) < names.size()
                                              ? static_cast<std::size_t>( requested )
                                              : names.size();
            if ( const auto picked = ValueCombo( label.c_str(), selected, names.size(),
                                                 [names]( std::size_t i ) { return std::string( names[i] ); } ) )
                SC::QualityState::SetOverride( parameter, static_cast<SC::ParameterValue>( *picked ) );
        }
        else
        {
            int value = m_DraggedParameter && m_DraggedParameter->first == parameter ? m_DraggedParameter->second
                                                                                      : requested;
            // A wide range (pool sizes, sample budgets) is dragged on a log scale.
            const ImGuiSliderFlags flags =
                 ImGuiSliderFlags_AlwaysClamp |
                 ( spec.Max - spec.Min > 256 && spec.Min >= 0 ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None );
            if ( ImGui::SliderInt( label.c_str(), &value, spec.Min, spec.Max, "%d", flags ) )
                m_DraggedParameter = std::make_pair( parameter, value );
            if ( ImGui::IsItemDeactivatedAfterEdit() && m_DraggedParameter &&
                 m_DraggedParameter->first == parameter )
            {
                SC::QualityState::SetOverride( parameter, m_DraggedParameter->second );
                m_DraggedParameter.reset();
            }
        }
        if ( spec.Reader )
            Utils::ImGuiUtilities::Tooltip(
                 std::format( "Read by {}. The level's value unless overridden; a slider applies on release.",
                              *spec.Reader )
                      .c_str() );
        if ( resolved.Overridden[static_cast<std::size_t>( parameter )] )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Reset" ) )
                SC::QualityState::ClearOverride( parameter );
        }
        ShowFallback( resolved, parameter );
        ImGui::PopID();
    }
} // namespace Desert::Editor

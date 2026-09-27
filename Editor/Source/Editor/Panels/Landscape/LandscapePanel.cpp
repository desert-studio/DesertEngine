// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/SLandscapeEditor.cpp:537-575 and
// LandscapeEditorDetailCustomization_ToolStrip / _Brushes, adapted: Slate's mode toolbar, tool strip and property
// grid become ImGui buttons and the editor's property rows; UObject meta (ShowForTools, UIMin/UIMax, ClampMin/
// ClampMax) comes from LandscapeToolProperties(); the tool order is LandscapeEdMode.cpp:216-269's Sculpt mode.
//
// Paint mode and its Target Layers section follow SLandscapeEditor's Paint tab and
// LandscapeEditorDetailCustomization_TargetLayers (list, current target, "+", Hardness / NoWeightBlend).
// Manage mode has New Landscape and UE's heightmap Import / Export
// (LandscapeEditorDetailCustomization_ImportExport: one file field, Import as a new landscape or into the existing
// one, Export all or the selected tiles). NOT PORTED, each for a stated reason. The Manage tools resize /
// components: nothing drives them yet. Alpha / Pattern / Component brush sets: the stroke maths has only UE's
// circle brush, so the brush row shows the one set that exists.

#include "LandscapePanel.hpp"

#include <Editor/Core/Commands/LandscapeLayerCommands.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Engine/Core/Scene.hpp>
#include <Common/Core/Constants.hpp>
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace Desert::Editor
{
    namespace
    {
        using Utils::ImGuiUtilities;

        struct ToolButton
        {
            Core::LandscapeTool Tool;
            const char*         Icon;
        };

        // UE's Sculpt mode strip, in its order (LandscapeEdMode.cpp:231-244); Visibility and Mask are absent
        // because this landscape has no visibility layer and no selection mask to paint.
        constexpr std::array<ToolButton, 10> kSculptTools = { {
             { Core::LandscapeTool::Sculpt, ICON_MDI_BRUSH },
             { Core::LandscapeTool::Erase, ICON_MDI_ERASER },
             { Core::LandscapeTool::Smooth, ICON_MDI_BLUR },
             { Core::LandscapeTool::Flatten, ICON_MDI_ARROW_COLLAPSE_VERTICAL },
             { Core::LandscapeTool::Ramp, ICON_MDI_SLOPE_UPHILL },
             { Core::LandscapeTool::Noise, ICON_MDI_GRAIN },
             { Core::LandscapeTool::Erosion, ICON_MDI_LANDSLIDE },
             { Core::LandscapeTool::HydroErosion, ICON_MDI_WEATHER_POURING },
             { Core::LandscapeTool::CopyPaste, ICON_MDI_CONTENT_COPY },
             { Core::LandscapeTool::Mirror, ICON_MDI_FLIP_HORIZONTAL },
        } };

        /// A button lit with the editor's one accent colour when @p active (UE's checked tool-strip entry).
        bool AccentButton( const char* label, bool active, const ImVec2& size )
        {
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ThemeManager::GetSelectedColor() );
            const bool pressed = ImGui::Button( label, size );
            if ( active )
                ImGui::PopStyleColor();
            return pressed;
        }

        void DrawProperty( const Core::LandscapeToolProperty& p, Core::LandscapeSculptSettings& s )
        {
            using Kind = Core::LandscapeToolProperty::Kind;
            ImGuiUtilities::BeginPropertyRow( p.Label );
            ImGui::SetNextItemWidth( -FLT_MIN );
            switch ( p.Type )
            {
                case Kind::Float:
                {
                    float*                 value = p.Float( s );
                    const ImGuiSliderFlags flags =
                         p.Logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
                    // The slider travels UE's UIMin..UIMax; Ctrl+click typing may go to ClampMin..ClampMax.
                    if ( ImGui::SliderFloat( "##v", value, p.UIMin, p.UIMax, p.UIMax < 2.0f ? "%.4g" : "%.1f",
                                             flags ) )
                        *value = std::clamp( *value, p.ClampMin, p.ClampMax );
                    break;
                }
                case Kind::Int:
                {
                    int32_t* value = p.Int( s );
                    int      v     = *value;
                    if ( ImGui::SliderInt( "##v", &v, static_cast<int>( p.UIMin ), static_cast<int>( p.UIMax ) ) )
                        *value = std::clamp( v, static_cast<int>( p.ClampMin ), static_cast<int>( p.ClampMax ) );
                    break;
                }
                case Kind::Bool:
                    ImGui::Checkbox( "##v", p.Bool( s ) );
                    break;
                case Kind::Choice:
                {
                    const int chosen = p.Chosen( s );
                    if ( ImGui::BeginCombo( "##v", p.Options[static_cast<size_t>( chosen )].c_str() ) )
                    {
                        for ( int i = 0; i < static_cast<int>( p.Options.size() ); ++i )
                            if ( ImGui::Selectable( p.Options[static_cast<size_t>( i )].c_str(), i == chosen ) )
                                p.Choose( s, i );
                        ImGui::EndCombo();
                    }
                    break;
                }
            }
            ImGuiUtilities::EndPropertyRow();
        }

        void PointRow( const char* label, const std::optional<glm::vec3>& point, const char* unset )
        {
            ImGuiUtilities::BeginPropertyRow( label );
            if ( point )
                ImGui::Text( "%.0f, %.0f, %.0f cm", point->x, point->y, point->z );
            else
                ImGui::TextDisabled( "%s", unset );
            ImGuiUtilities::EndPropertyRow();
        }
    } // namespace

    LandscapePanel::LandscapePanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Landscape", /*showPanel=*/false ) // contextual: the mode opens it
           ,
           m_Scene( scene )
    {
    }

    bool LandscapePanel::IsRelevant() const
    {
        return Core::ViewportMode::Get() == Core::EditorMode::Landscape;
    }

    void LandscapePanel::OnUIRender()
    {
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 5.0f ) );

        // UE's mode row (Manage / Sculpt / Paint): the modes with tools behind them are offered.
        auto& state = Core::LandscapeSculptState::Get();
        if ( AccentButton( ICON_MDI_PLUS_BOX_OUTLINE "  Manage", state.Mode == Core::LandscapeEdMode::Manage,
                           ImVec2( 0.0f, 0.0f ) ) )
            state.Mode = Core::LandscapeEdMode::Manage;
        ImGui::SameLine();
        if ( AccentButton( ICON_MDI_TERRAIN "  Sculpt", state.Mode == Core::LandscapeEdMode::Sculpt,
                           ImVec2( 0.0f, 0.0f ) ) )
            state.Mode = Core::LandscapeEdMode::Sculpt;
        ImGui::SameLine();
        if ( AccentButton( ICON_MDI_BRUSH "  Paint", state.Mode == Core::LandscapeEdMode::Paint,
                           ImVec2( 0.0f, 0.0f ) ) )
            state.Mode = Core::LandscapeEdMode::Paint;
        ImGui::Separator();

        if ( state.Mode == Core::LandscapeEdMode::Manage )
        {
            DrawNewLandscape();
            DrawHeightmapFile();
        }
        else if ( state.Mode == Core::LandscapeEdMode::Paint )
        {
            DrawPaintSettings();
            DrawTargetLayers();
            DrawBrushSettings();
        }
        else
        {
            DrawToolStrip();
            ImGui::Spacing();
            DrawToolSettings();
            DrawBrushSettings();
        }

        ImGui::PopStyleVar( 2 );
    }

    // UE's New Landscape (Manage mode): the frame, UE's size readouts, the fill and the whole-map erosion passes.
    void LandscapePanel::DrawNewLandscape()
    {
        namespace L = World::Landscape;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_PLUS_BOX_OUTLINE "  New Landscape", true, "" ) )
            return;
        auto& s = Core::LandscapeSculptState::Get().NewLandscape;
        ImGui::DragFloat3( "Location (cm)", &s.LocationCm.x, 100.0f );
        if ( ImGui::BeginCombo( "Section Size", ( std::to_string( s.QuadsPerTile ) + "x" +
                                                  std::to_string( s.QuadsPerTile ) + " Quads" )
                                                     .c_str() ) )
        {
            for ( const uint32_t q : L::kLandscapeTileQuadsValues )
                if ( ImGui::Selectable( ( std::to_string( q ) + "x" + std::to_string( q ) + " Quads" ).c_str(),
                                        q == s.QuadsPerTile ) )
                    s.QuadsPerTile = q;
            ImGui::EndCombo();
        }
        int tiles[2] = { s.TilesX, s.TilesZ };
        if ( ImGui::DragInt2( "Number of Components", tiles, 0.25f, 1, L::kLandscapeMaxTilesPerSide ) )
        {
            s.TilesX = tiles[0];
            s.TilesZ = tiles[1];
        }
        s.TilesX = L::ClampLandscapeTileCount( s.TilesX, s.QuadsPerTile );
        s.TilesZ = L::ClampLandscapeTileCount( s.TilesZ, s.QuadsPerTile );
        ImGui::DragFloat( "Scale XY (cm)", &s.SpacingCm, 1.0f, 1.0f, 10000.0f );
        ImGui::DragFloat( "Scale Z", &s.ZScale, 1.0f, 1.0f, 10000.0f );
        ImGui::TextDisabled( "Overall Resolution %u x %u, %d components",
                             static_cast<unsigned>( s.TilesX ) * s.QuadsPerTile + 1u,
                             static_cast<unsigned>( s.TilesZ ) * s.QuadsPerTile + 1u, s.TilesX * s.TilesZ );

        int fill = static_cast<int>( s.Fill );
        ImGui::Combo( "Fill", &fill, "Flat\0Noise\0" );
        s.Fill = static_cast<L::LandscapeGenerateFill>( fill );
        if ( s.Fill == L::LandscapeGenerateFill::Noise )
        {
            int seed = static_cast<int>( s.Seed );
            if ( ImGui::InputInt( "Seed", &seed ) )
                s.Seed = static_cast<uint32_t>( std::max( seed, 0 ) );
            ImGui::DragFloat( "Noise Height (cm)", &s.NoiseHeightCm, 10.0f, 0.0f, 100000.0f );
            ImGui::SliderFloat( "Noise Scale", &s.NoiseScale, L::kLandscapeMinNoiseScale,
                                L::kLandscapeMaxNoiseScale );
        }
        ImGui::Checkbox( "Erosion", &s.Erosion );
        if ( s.Erosion )
        {
            ImGui::SliderInt( "Threshold", &s.ErosionSettings.Threshold, 0, L::kLandscapeMaxErosionThreshold );
            ImGui::SliderInt( "Iterations", &s.ErosionSettings.Iterations, 1, L::kLandscapeMaxErosionIterations );
        }
        ImGui::Checkbox( "Hydro Erosion", &s.HydroErosion );
        if ( s.HydroErosion )
        {
            ImGui::SliderInt( "Rain Amount", &s.HydroSettings.RainAmount, 1, L::kLandscapeMaxRainAmount );
            ImGui::SliderInt( "Hydro Iterations", &s.HydroSettings.Iterations, 1,
                              L::kLandscapeMaxErosionIterations );
        }
        if ( s.Erosion || s.HydroErosion )
            ImGui::SliderFloat( "Erosion Strength", &s.ErosionStrength, 0.0f, 1.0f );

        // The run is on the JobSystem; the editor applies it when it finishes (EditorLayer). While it runs, Create
        // is closed and says why, and the run can be cancelled.
        if ( Commands::IsCreatingLandscape() )
        {
            const float fraction = Commands::CreateLandscapeFraction();
            ImGui::ProgressBar(
                 fraction, ImVec2( -FLT_MIN, 0.0f ),
                 ( "Generating " + std::to_string( static_cast<int>( fraction * 100.0f ) ) + " %" ).c_str() );
            ImGui::BeginDisabled();
            ImGui::Button( ICON_MDI_CHECK "  Create", ImVec2( -FLT_MIN, 0.0f ) );
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                 "A landscape is being generated; Create opens when it finishes or is cancelled." );
            if ( ImGui::Button( ICON_MDI_CLOSE "  Cancel", ImVec2( -FLT_MIN, 0.0f ) ) )
                Commands::CancelCreateLandscape();
        }
        else if ( ImGui::Button( ICON_MDI_CHECK "  Create", ImVec2( -FLT_MIN, 0.0f ) ) )
        {
            auto started = Commands::StartCreateLandscape( m_Scene.lock(), s );
            if ( !started.IsSuccess() )
                ToastManager::Push( started.GetError(), ToastLevel::Error, 6.0f );
        }
    }

    // UE's Import / Export (Manage mode): 16-bit PNG or RAW, the landscape's own size, one undo step per import.
    void LandscapePanel::DrawHeightmapFile()
    {
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Heightmap File", true, "" ) )
            return;
        ImGui::SetNextItemWidth( -FLT_MIN );
        ImGui::InputText( "##HeightmapPath", m_HeightmapPath.data(), m_HeightmapPath.size() );
        ImGui::TextDisabled( "16-bit .png, or .r16 / .raw; relative paths are under Assets." );
        std::filesystem::path path( m_HeightmapPath.data() );
        if ( path.is_relative() )
            path = Common::Constants::Path::ASSETS_PATH / path;
        const auto scene  = m_Scene.lock();
        const auto report = []( const Common::BoolResultStr& r )
        {
            if ( !r.IsSuccess() )
                ToastManager::Push( r.GetError(), ToastLevel::Error, 6.0f );
        };
        if ( ImGui::Button( ICON_MDI_PLUS_BOX_OUTLINE "  Import as New Landscape", ImVec2( -FLT_MIN, 0.0f ) ) )
        {
            auto made = Commands::ImportLandscapeHeightmapAsNew( scene, path,
                                                                 Core::LandscapeSculptState::Get().NewLandscape );
            if ( !made.IsSuccess() )
                ToastManager::Push( made.GetError(), ToastLevel::Error, 6.0f );
        }
        if ( ImGui::Button( ICON_MDI_FILE_IMPORT_OUTLINE "  Import into Landscape", ImVec2( -FLT_MIN, 0.0f ) ) )
            report( Commands::ImportLandscapeHeightmap( scene, path ) );
        if ( ImGui::Button( ICON_MDI_FILE_EXPORT_OUTLINE "  Export Landscape", ImVec2( -FLT_MIN, 0.0f ) ) )
            report( Commands::ExportLandscapeHeightmap( scene, path, false ) );
        if ( ImGui::Button( ICON_MDI_FILE_EXPORT_OUTLINE "  Export Selected Tiles", ImVec2( -FLT_MIN, 0.0f ) ) )
            report( Commands::ExportLandscapeHeightmap( scene, path, true ) );
    }

    void LandscapePanel::DrawToolStrip()
    {
        auto& settings = Core::LandscapeSculptState::Get().Settings;
        // Every button is as wide as the widest name, so "Hydro Erosion" and "Copy/Paste" are not clipped.
        float widest = 0.0f;
        for ( const auto& button : kSculptTools )
            widest = std::max( widest, ImGui::CalcTextSize( Core::LandscapeToolName( button.Tool ) ).x );
        const ImVec2 size( std::max( 72.0f, widest + 2.0f * ImGui::GetStyle().FramePadding.x ), 50.0f );
        const float  right   = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float  spacing = ImGui::GetStyle().ItemSpacing.x;
        for ( size_t i = 0; i < kSculptTools.size(); ++i )
        {
            const auto& button = kSculptTools[i];
            char        label[64];
            std::snprintf( label, sizeof( label ), "%s\n%s##tool%zu", button.Icon,
                           Core::LandscapeToolName( button.Tool ), i );
            if ( AccentButton( label, settings.Tool == button.Tool, size ) )
                settings.Tool = button.Tool;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", Core::LandscapeToolName( button.Tool ) );
            // Wrap like UE's strip: the next button goes on this line only if it fits.
            if ( i + 1 < kSculptTools.size() && ImGui::GetItemRectMax().x + spacing + size.x <= right )
                ImGui::SameLine();
        }
    }

    void LandscapePanel::DrawToolSettings()
    {
        auto& state    = Core::LandscapeSculptState::Get();
        auto& settings = state.Settings;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_COG "  Tool Settings", true,
                                             Core::LandscapeToolName( settings.Tool ) ) )
            return;
        ImGuiUtilities::ResetPropertyRows();
        const auto properties = Core::LandscapeToolProperties();
        for ( size_t i = 0; i < properties.size(); ++i )
        {
            if ( !properties[i].Shown( settings ) )
                continue;
            ImGui::PushID( static_cast<int>( i ) );
            DrawProperty( properties[i], settings );
            ImGui::PopID();
        }

        // The tool's points and buffers (UE draws them in the viewport and in its own detail rows).
        switch ( settings.Tool )
        {
            case Core::LandscapeTool::Ramp:
            {
                const auto& ramp = state.RampPoints;
                PointRow( "Start", ramp.NumPoints > 0 ? std::optional( ramp.Points[0] ) : std::nullopt,
                          "click the landscape" );
                PointRow( "End", ramp.NumPoints > 1 ? std::optional( ramp.Points[1] ) : std::nullopt,
                          "click or drag from the start" );
                break;
            }
            case Core::LandscapeTool::Mirror:
                PointRow( "Mirror Point", state.MirrorPoint, "landscape centre" );
                break;
            case Core::LandscapeTool::CopyPaste:
                PointRow( "Corner A", state.CopyCornerA, "not set" );
                PointRow( "Corner B", state.CopyCornerB, "not set" );
                ImGuiUtilities::BeginPropertyRow( "Copied" );
                ImGui::Text( "%d x %d samples", state.CopyBuffer.SizeX, state.CopyBuffer.SizeZ );
                ImGuiUtilities::EndPropertyRow();
                break;
            default:
                break;
        }

        // The tool's actions (UE's "Add Ramp" / "Reset", Mirror's Apply, the gizmo's Copy / Paste): the rows
        // of LandscapeToolControls() that post a request, so each button is also a palette command.
        int id = 0;
        for ( const auto& control : Core::LandscapeToolControls() )
        {
            if ( control.Request == Core::LandscapeStrokeRequest::None || !control.Shown( settings ) )
                continue;
            ImGui::PushID( id++ );
            if ( ImGui::Button( control.Label.c_str(), ImVec2( -FLT_MIN, 0.0f ) ) )
                state.Request = control.Request;
            ImGui::PopID();
        }
        // Wrapped: the Ramp hint is wider than the default dock column.
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] );
        ImGui::TextWrapped( "%s", settings.Tool == Core::LandscapeTool::Ramp
                                       ? "LMB sets the start, then the end; Apply builds the ramp"
                                       : "LMB applies the tool, Shift+LMB inverts it" );
        ImGui::PopStyleColor();
    }

    void LandscapePanel::DrawBrushSettings()
    {
        auto& brush = Core::LandscapeSculptState::Get().Settings.Brush;
        if ( !Core::LandscapeToolUsesBrush( Core::LandscapeSculptState::Get().Settings.Tool ) )
            return;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_BRUSH "  Brush Settings" ) )
            return;
        ImGuiUtilities::ResetPropertyRows();

        // UE's brush-set row; the circle is the one set the stroke maths has.
        ImGuiUtilities::BeginPropertyRow( "Brush Type" );
        AccentButton( ICON_MDI_CIRCLE_OUTLINE "  Circle", true, ImVec2( 0.0f, 0.0f ) );
        ImGuiUtilities::EndPropertyRow();

        ImGuiUtilities::BeginPropertyRow( "Brush Falloff Type" );
        int         id      = 0;
        const float right   = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for ( const auto shape : Core::LandscapeFalloffShapes() )
        {
            // Wrap inside the value column: four names do not fit one line of the default dock width.
            const float width = ImGui::CalcTextSize( Core::LandscapeFalloffName( shape ) ).x +
                                2.0f * ImGui::GetStyle().FramePadding.x;
            if ( id > 0 && ImGui::GetItemRectMax().x + spacing + width <= right )
                ImGui::SameLine();
            ImGui::PushID( id++ );
            if ( AccentButton( Core::LandscapeFalloffName( shape ), brush.Shape == shape, ImVec2( 0.0f, 0.0f ) ) )
                brush.Shape = shape;
            ImGui::PopID();
        }
        ImGuiUtilities::EndPropertyRow();

        // BrushRadius / BrushFalloff (LandscapeEditorObject.h:677-690): UI 1..8192 with SliderExponent 3,
        // clamp to 65536; this tool's own floor (kLandscapeMinRadiusCm) is tighter than UE's 1.
        ImGuiUtilities::BeginPropertyRow( "Brush Size" );
        ImGui::SetNextItemWidth( -FLT_MIN );
        if ( ImGui::SliderFloat( "##radius", &brush.RadiusCm, Core::kLandscapeMinRadiusCm, 8192.0f, "%.0f cm",
                                 ImGuiSliderFlags_Logarithmic ) )
            brush.RadiusCm =
                 std::clamp( brush.RadiusCm, Core::kLandscapeMinRadiusCm, Core::kLandscapeMaxRadiusCm );
        ImGuiUtilities::EndPropertyRow();

        ImGuiUtilities::BeginPropertyRow( "Brush Falloff" );
        ImGui::SetNextItemWidth( -FLT_MIN );
        if ( ImGui::SliderFloat( "##falloff", &brush.FalloffFraction, 0.0f, 1.0f, "%.2f" ) )
            brush.FalloffFraction = std::clamp( brush.FalloffFraction, 0.0f, 1.0f );
        ImGuiUtilities::EndPropertyRow();
    }
    void LandscapePanel::DrawPaintSettings()
    {
        auto& paint = Core::LandscapeSculptState::Get().Paint;
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_COG "  Tool Settings", true, "Paint" ) )
            return;
        ImGuiUtilities::ResetPropertyRows();
        ImGuiUtilities::BeginPropertyRow( "Use Target Value" );
        ImGui::Checkbox( "##useTarget", &paint.UseTargetValue );
        ImGuiUtilities::EndPropertyRow();
        if ( paint.UseTargetValue )
        {
            ImGuiUtilities::BeginPropertyRow( "Target Value" );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "##targetValue", &paint.TargetValue, 0.0f, 1.0f, "%.3f",
                                ImGuiSliderFlags_AlwaysClamp );
            ImGuiUtilities::EndPropertyRow();
        }
        ImGuiUtilities::BeginPropertyRow( "Disable Startup Slowdown" );
        ImGui::Checkbox( "##noSlowdown", &paint.DisableStartupSlowdown );
        ImGuiUtilities::EndPropertyRow();
    }

    void LandscapePanel::DrawTargetLayers()
    {
        if ( !ImGuiUtilities::SectionHeader( ICON_MDI_LAYERS "  Target Layers", true, "" ) )
            return;
        const auto scene = m_Scene.lock();
        if ( !scene )
        {
            ImGui::TextDisabled( "no scene" );
            return;
        }
        auto&      registry  = scene->GetRegistry();
        const auto landscape = ECS::FirstLandscape( registry );
        const auto root =
             landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
        if ( root == entt::null )
        {
            ImGui::TextDisabled( "the scene has no loaded landscape" );
            return;
        }
        const auto layers  = registry.get<ECS::LandscapeComponent>( root ).Layers; // a copy: commands edit it
        auto&      paint   = Core::LandscapeSculptState::Get().Paint;
        auto&      service = *Runtime::ResourceRegistry::GetLandscapeLayerInfoService();

        if ( ImGui::Button( ICON_MDI_PLUS "  Create Layer Info" ) )
        {
            auto added = Commands::AddLandscapeLayer( scene );
            if ( !added.IsSuccess() )
                ToastManager::Push( added.GetError(), ToastLevel::Error, 6.0f );
            else if ( paint.Layer.empty() )
                paint.Layer = added.GetValue();
            return; // the command already recorded this change
        }
        if ( layers.empty() )
            ImGui::TextDisabled( "no target layers: create a layer info to paint" );

        const auto& rows = Assets::ContentRegistry::Rows( Common::Content::ContentKind::LandscapeLayerInfo );
        for ( size_t i = 0; i < layers.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            const Assets::AssetHandle handle = layers[i];
            const auto*               info   = service.Get( handle );

            // The asset slot (UE: the target layer's Layer Info object picker).
            std::string current = "(missing)";
            for ( const auto& row : rows )
                if ( row.Handle == handle )
                    current = row.Path.stem().string();
            ImGui::SetNextItemWidth( -ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x );
            if ( ImGui::BeginCombo( "##layerInfo", current.c_str() ) )
            {
                for ( const auto& row : rows )
                    if ( ImGui::Selectable( row.Path.stem().string().c_str(), row.Handle == handle ) &&
                         row.Handle != handle )
                        if ( auto assigned = Commands::AssignLandscapeLayer( scene, i, row.Handle ); !assigned )
                            ToastManager::Push( assigned.GetError(), ToastLevel::Error, 6.0f );
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CLOSE ) )
            {
                if ( auto removed = Commands::RemoveLandscapeLayer( scene, i ); !removed )
                    ToastManager::Push( removed.GetError(), ToastLevel::Error, 6.0f );
                ImGui::PopID();
                return;
            }
            if ( !info )
            {
                const bool pending =
                     service.StateOf( handle ) == Runtime::LandscapeLayerInfoService::State::Pending;
                ImGui::TextDisabled( "%s", pending ? "loading..." : service.ErrorOf( handle ).c_str() );
                ImGui::PopID();
                continue;
            }

            // Edits go to a copy and are saved to the asset when the widget is released (FO-1's pattern); the
            // copy lives across frames only while this layer is the one being edited.
            Assets::Serialization::LandscapeLayerInfoData local =
                 m_LayerEdit && m_LayerEdit->first == handle ? m_LayerEdit->second : *info;
            auto& edit   = local;
            bool  commit = false;
            ImGui::ColorEdit3( "##swatch", &edit.LayerUsageDebugColor.x, ImGuiColorEditFlags_NoInputs );
            commit = commit || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            if ( ImGui::Selectable( info->LayerName.c_str(), paint.Layer == info->LayerName ) )
                paint.Layer = info->LayerName;
            ImGuiUtilities::ResetPropertyRows();
            ImGuiUtilities::BeginPropertyRow( "Hardness" );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "##hardness", &edit.Hardness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
            commit = commit || ImGui::IsItemDeactivatedAfterEdit();
            ImGuiUtilities::EndPropertyRow();
            ImGuiUtilities::BeginPropertyRow( "No Weight Blend" );
            if ( ImGui::Checkbox( "##noBlend", &edit.NoWeightBlend ) )
                commit = true;
            ImGuiUtilities::EndPropertyRow();
            if ( !( edit == *info ) )
                m_LayerEdit.emplace( handle, edit );
            if ( commit && m_LayerEdit && m_LayerEdit->first == handle )
            {
                if ( auto saved = service.Save( handle, m_LayerEdit->second ); !saved )
                    ToastManager::Push( saved.GetError(), ToastLevel::Error, 6.0f );
                m_LayerEdit.reset();
            }
            ImGui::PopID();
        }
    }
} // namespace Desert::Editor

#pragma once

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    /**
     * @brief The Landscape mode's Sculpt / Smooth / Flatten / Noise / Erase / Ramp / Erosion / Hydro Erosion tool
     * settings (UE's ULandscapeEditorObject subset) and the ONE table the tool panel draws its widgets from and
     * the command palette offers as commands.
     *
     * WHY ONE TABLE. Synthetic mouse input is closed on this machine, so a setting only a hand can change is a
     * setting no unattended run can photograph. The panel draws NOTHING but the rows of LandscapeToolControls()
     * (plus read-outs), and the palette offers exactly those rows: a widget without a command cannot be written.
     */
    enum class LandscapeTool : uint8_t
    {
        Sculpt,
        Smooth,
        Flatten,
        Noise,
        Erase,
        Ramp,
        Erosion,
        HydroErosion,
    };

    inline const char* LandscapeToolName( LandscapeTool tool )
    {
        switch ( tool )
        {
            case LandscapeTool::Sculpt:
                return "Sculpt";
            case LandscapeTool::Smooth:
                return "Smooth";
            case LandscapeTool::Flatten:
                return "Flatten";
            case LandscapeTool::Noise:
                return "Noise";
            case LandscapeTool::Erase:
                return "Erase";
            case LandscapeTool::Ramp:
                return "Ramp";
            case LandscapeTool::Erosion:
                return "Erosion";
            case LandscapeTool::HydroErosion:
                return "Hydro Erosion";
        }
        return "Unknown";
    }

    inline const char* LandscapeFlattenModeName( World::Landscape::LandscapeFlattenMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapeFlattenMode::Both:
                return "Both";
            case World::Landscape::LandscapeFlattenMode::Raise:
                return "Raise";
            case World::Landscape::LandscapeFlattenMode::Lower:
                return "Lower";
            case World::Landscape::LandscapeFlattenMode::Interval:
                return "Interval";
            case World::Landscape::LandscapeFlattenMode::Terrace:
                return "Terrace";
        }
        return "Unknown";
    }

    inline const char* LandscapeNoiseModeName( World::Landscape::LandscapeNoiseMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapeNoiseMode::Both:
                return "Both";
            case World::Landscape::LandscapeNoiseMode::Add:
                return "Add";
            case World::Landscape::LandscapeNoiseMode::Sub:
                return "Sub";
        }
        return "Unknown";
    }

    inline const char* LandscapeErosionNoiseModeName( World::Landscape::LandscapeErosionNoiseMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapeErosionNoiseMode::Both:
                return "Both";
            case World::Landscape::LandscapeErosionNoiseMode::Raise:
                return "Raise";
            case World::Landscape::LandscapeErosionNoiseMode::Lower:
                return "Lower";
        }
        return "Unknown";
    }

    inline const char* LandscapeRainModeName( World::Landscape::LandscapeRainMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapeRainMode::Both:
                return "Both";
            case World::Landscape::LandscapeRainMode::Positive:
                return "Positive";
        }
        return "Unknown";
    }

    inline const char* LandscapeRampModeName( World::Landscape::LandscapeRampMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapeRampMode::Both:
                return "Both";
            case World::Landscape::LandscapeRampMode::Raise:
                return "Raise";
            case World::Landscape::LandscapeRampMode::Lower:
                return "Lower";
        }
        return "Unknown";
    }

    inline const char* LandscapeFalloffName( World::Landscape::LandscapeBrushFalloff shape )
    {
        switch ( shape )
        {
            case World::Landscape::LandscapeBrushFalloff::Linear:
                return "Linear";
            case World::Landscape::LandscapeBrushFalloff::Smooth:
                return "Smooth";
            case World::Landscape::LandscapeBrushFalloff::Spherical:
                return "Spherical";
            case World::Landscape::LandscapeBrushFalloff::Tip:
                return "Tip";
        }
        return "Unknown";
    }

    inline constexpr float kLandscapeMinRadiusCm = 100.0f;
    inline constexpr float kLandscapeMaxRadiusCm = 65536.0f;
    inline constexpr float kLandscapeMinStrength = 0.05f;
    /// UE's ToolStrength ClampMax; Smooth clamps what it uses to 1 itself, as UE does.
    inline constexpr float kLandscapeMaxStrength = 10.0f;

    struct LandscapeSculptSettings
    {
        LandscapeTool                              Tool = LandscapeTool::Sculpt;
        World::Landscape::LandscapeBrushSettings   Brush;
        World::Landscape::LandscapeSmoothSettings  Smooth;
        World::Landscape::LandscapeFlattenSettings Flatten;
        World::Landscape::LandscapeNoiseSettings   Noise;
        World::Landscape::LandscapeRampSettings    Ramp;
        World::Landscape::LandscapeErosionSettings      Erosion;
        World::Landscape::LandscapeHydroErosionSettings HydroErosion;
    };

    /// A stroke asked for from the palette: one step at the viewport centre, then the stroke ends. The ramp's
    /// requests stand for UE's clicks on the landscape (a point each) and its Enter / Escape (apply / reset).
    enum class LandscapeStrokeRequest : uint8_t
    {
        None,
        Raise,
        Lower,
        RampStart,
        RampEnd,
        RampApply,
        RampReset,
    };

    struct LandscapeSculptState
    {
        LandscapeSculptSettings Settings;
        LandscapeStrokeRequest  Request = LandscapeStrokeRequest::None;
        /// UE's FLandscapeToolRamp::Points, in world cm; applying keeps them, as UE does until the tool is reset.
        std::optional<glm::vec3> RampStart;
        std::optional<glm::vec3> RampEnd;

        static LandscapeSculptState& Get()
        {
            static LandscapeSculptState s_State;
            return s_State;
        }
    };

    struct LandscapeToolControl
    {
        /// The palette label under the group "Landscape", and the panel button's text.
        std::string Label;
        /// The panel row the button sits on (a read-out names it); the palette ignores it.
        const char*                                     Row = "";
        std::function<void( LandscapeSculptSettings& )> Apply;
        /// Whether the button is offered for the current tool (UE's ShowForTools).
        std::function<bool( const LandscapeSculptSettings& )> Shown;
        /// Not None: the row is an action at the viewport centre, not a setting — Apply is empty and both the
        /// panel and the palette post this request, which the viewport tool serves on its next update.
        LandscapeStrokeRequest Request = LandscapeStrokeRequest::None;
    };

    inline std::vector<LandscapeToolControl> LandscapeToolControls()
    {
        using World::Landscape::LandscapeBrushFalloff;
        auto always = []( const LandscapeSculptSettings& ) { return true; };
        using World::Landscape::LandscapeFlattenMode;
        using World::Landscape::LandscapeNoiseMode;
        auto smooth  = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Smooth; };
        auto flatten = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Flatten; };
        auto noise   = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Noise; };
        auto ramp    = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Ramp; };

        std::vector<LandscapeToolControl> controls;
        for ( const LandscapeTool tool :
              { LandscapeTool::Sculpt, LandscapeTool::Smooth, LandscapeTool::Flatten, LandscapeTool::Noise,
                LandscapeTool::Erase, LandscapeTool::Ramp, LandscapeTool::Erosion, LandscapeTool::HydroErosion } )
            controls.push_back( { std::string( "Tool: " ) + LandscapeToolName( tool ), "Tool",
                                  [tool]( LandscapeSculptSettings& s ) { s.Tool = tool; }, always } );

        controls.push_back( { "Brush radius: larger", "Radius", []( LandscapeSculptSettings& s )
                              { s.Brush.RadiusCm = std::min( s.Brush.RadiusCm * 1.25f, kLandscapeMaxRadiusCm ); },
                              always } );
        controls.push_back( { "Brush radius: smaller", "Radius", []( LandscapeSculptSettings& s )
                              { s.Brush.RadiusCm = std::max( s.Brush.RadiusCm / 1.25f, kLandscapeMinRadiusCm ); },
                              always } );
        controls.push_back( { "Brush falloff: larger", "Falloff", []( LandscapeSculptSettings& s )
                              { s.Brush.FalloffFraction = std::min( s.Brush.FalloffFraction + 0.1f, 1.0f ); },
                              always } );
        controls.push_back( { "Brush falloff: smaller", "Falloff", []( LandscapeSculptSettings& s )
                              { s.Brush.FalloffFraction = std::max( s.Brush.FalloffFraction - 0.1f, 0.0f ); },
                              always } );
        for ( const LandscapeBrushFalloff shape :
              { LandscapeBrushFalloff::Linear, LandscapeBrushFalloff::Smooth, LandscapeBrushFalloff::Spherical,
                LandscapeBrushFalloff::Tip } )
            controls.push_back( { std::string( "Brush falloff shape: " ) + LandscapeFalloffName( shape ), "Shape",
                                  [shape]( LandscapeSculptSettings& s ) { s.Brush.Shape = shape; }, always } );
        controls.push_back( { "Tool strength: larger", "Strength", []( LandscapeSculptSettings& s )
                              { s.Brush.Strength = std::min( s.Brush.Strength + 0.1f, kLandscapeMaxStrength ); },
                              always } );
        controls.push_back( { "Tool strength: smaller", "Strength", []( LandscapeSculptSettings& s )
                              { s.Brush.Strength = std::max( s.Brush.Strength - 0.1f, kLandscapeMinStrength ); },
                              always } );
        controls.push_back( { "Smooth filter radius: larger", "Filter radius",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Smooth.FilterKernelRadius =
                                       std::min( s.Smooth.FilterKernelRadius + 1,
                                                 World::Landscape::kLandscapeSmoothMaxRadius );
                              },
                              smooth } );
        controls.push_back( { "Smooth filter radius: smaller", "Filter radius",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Smooth.FilterKernelRadius =
                                       std::max( s.Smooth.FilterKernelRadius - 1,
                                                 World::Landscape::kLandscapeSmoothMinRadius );
                              },
                              smooth } );
        controls.push_back( { "Detail smooth: on", "Detail smooth",
                              []( LandscapeSculptSettings& s ) { s.Smooth.DetailSmooth = true; }, smooth } );
        controls.push_back( { "Detail smooth: off", "Detail smooth",
                              []( LandscapeSculptSettings& s ) { s.Smooth.DetailSmooth = false; }, smooth } );
        controls.push_back( { "Detail scale: larger", "Detail scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.Smooth.DetailScale = std::min( s.Smooth.DetailScale + 0.1f,
                                                                   World::Landscape::kLandscapeMaxDetailScale );
                              },
                              smooth } );
        controls.push_back( { "Detail scale: smaller", "Detail scale", []( LandscapeSculptSettings& s )
                              { s.Smooth.DetailScale = std::max( s.Smooth.DetailScale - 0.1f, 0.0f ); },
                              smooth } );

        for ( const LandscapeFlattenMode mode :
              { LandscapeFlattenMode::Both, LandscapeFlattenMode::Raise, LandscapeFlattenMode::Lower,
                LandscapeFlattenMode::Interval, LandscapeFlattenMode::Terrace } )
            controls.push_back( { std::string( "Flatten mode: " ) + LandscapeFlattenModeName( mode ),
                                  "Flatten mode", [mode]( LandscapeSculptSettings& s ) { s.Flatten.Mode = mode; },
                                  flatten } );
        controls.push_back( { "Slope flatten: on", "Slope flatten",
                              []( LandscapeSculptSettings& s ) { s.Flatten.UseSlopeFlatten = true; }, flatten } );
        controls.push_back( { "Slope flatten: off", "Slope flatten",
                              []( LandscapeSculptSettings& s ) { s.Flatten.UseSlopeFlatten = false; }, flatten } );
        controls.push_back( { "Pick value per apply: on", "Pick per apply", []( LandscapeSculptSettings& s )
                              { s.Flatten.PickValuePerApply = true; }, flatten } );
        controls.push_back( { "Pick value per apply: off", "Pick per apply", []( LandscapeSculptSettings& s )
                              { s.Flatten.PickValuePerApply = false; }, flatten } );
        controls.push_back( { "Terrace interval: larger", "Terrace interval",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Flatten.TerraceIntervalCm =
                                       std::min( s.Flatten.TerraceIntervalCm * 2.0f,
                                                 World::Landscape::kLandscapeMaxTerraceIntervalCm );
                              },
                              flatten } );
        controls.push_back( { "Terrace interval: smaller", "Terrace interval",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Flatten.TerraceIntervalCm =
                                       std::max( s.Flatten.TerraceIntervalCm / 2.0f,
                                                 World::Landscape::kLandscapeMinTerraceIntervalCm );
                              },
                              flatten } );
        controls.push_back( { "Terrace smooth: larger", "Terrace smooth",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Flatten.TerraceSmooth =
                                       std::min( s.Flatten.TerraceSmooth * 4.0f,
                                                 World::Landscape::kLandscapeMaxTerraceSmooth );
                              },
                              flatten } );
        controls.push_back( { "Terrace smooth: smaller", "Terrace smooth",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.Flatten.TerraceSmooth =
                                       std::max( s.Flatten.TerraceSmooth / 4.0f,
                                                 World::Landscape::kLandscapeMinTerraceSmooth );
                              },
                              flatten } );
        for ( const LandscapeNoiseMode mode :
              { LandscapeNoiseMode::Both, LandscapeNoiseMode::Add, LandscapeNoiseMode::Sub } )
            controls.push_back( { std::string( "Noise mode: " ) + LandscapeNoiseModeName( mode ), "Noise mode",
                                  [mode]( LandscapeSculptSettings& s ) { s.Noise.Mode = mode; }, noise } );
        controls.push_back( { "Noise scale: larger", "Noise scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.Noise.NoiseScale = std::min( s.Noise.NoiseScale * 2.0f,
                                                                 World::Landscape::kLandscapeMaxNoiseScale );
                              },
                              noise } );
        controls.push_back( { "Noise scale: smaller", "Noise scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.Noise.NoiseScale = std::max( s.Noise.NoiseScale / 2.0f,
                                                                 World::Landscape::kLandscapeMinNoiseScale );
                              },
                              noise } );
        using World::Landscape::LandscapeRampMode;
        for ( const LandscapeRampMode mode :
              { LandscapeRampMode::Both, LandscapeRampMode::Raise, LandscapeRampMode::Lower } )
            controls.push_back( { std::string( "Ramp mode: " ) + LandscapeRampModeName( mode ), "Ramp mode",
                                  [mode]( LandscapeSculptSettings& s ) { s.Ramp.Mode = mode; }, ramp } );
        controls.push_back( { "Ramp width: larger", "Ramp width",
                              []( LandscapeSculptSettings& s ) {
                                  s.Ramp.WidthCm = std::min( s.Ramp.WidthCm * 1.25f,
                                                             World::Landscape::kLandscapeMaxRampWidthUiCm );
                              },
                              ramp } );
        controls.push_back( { "Ramp width: smaller", "Ramp width",
                              []( LandscapeSculptSettings& s ) {
                                  s.Ramp.WidthCm = std::max( s.Ramp.WidthCm / 1.25f,
                                                             World::Landscape::kLandscapeMinRampWidthCm );
                              },
                              ramp } );
        controls.push_back( { "Ramp side falloff: larger", "Side falloff", []( LandscapeSculptSettings& s )
                              { s.Ramp.SideFalloff = std::min( s.Ramp.SideFalloff + 0.1f, 1.0f ); }, ramp } );
        controls.push_back( { "Ramp side falloff: smaller", "Side falloff", []( LandscapeSculptSettings& s )
                              { s.Ramp.SideFalloff = std::max( s.Ramp.SideFalloff - 0.1f, 0.0f ); }, ramp } );
        controls.push_back(
             { "Ramp: set start at viewport centre", "Ramp", {}, ramp, LandscapeStrokeRequest::RampStart } );
        controls.push_back(
             { "Ramp: set end at viewport centre", "Ramp", {}, ramp, LandscapeStrokeRequest::RampEnd } );
        controls.push_back( { "Ramp: apply", "Ramp", {}, ramp, LandscapeStrokeRequest::RampApply } );
        controls.push_back( { "Ramp: reset", "Ramp", {}, ramp, LandscapeStrokeRequest::RampReset } );

        namespace L  = World::Landscape;
        auto erosion = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Erosion; };
        auto hydro   = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::HydroErosion; };
        controls.push_back(
             { "Erosion threshold: larger", "Erosion threshold", []( LandscapeSculptSettings& s )
               { s.Erosion.Threshold = std::min( s.Erosion.Threshold + 16, L::kLandscapeMaxErosionThreshold ); },
               erosion } );
        controls.push_back( { "Erosion threshold: smaller", "Erosion threshold", []( LandscapeSculptSettings& s )
                              { s.Erosion.Threshold = std::max( s.Erosion.Threshold - 16, 0 ); }, erosion } );
        controls.push_back(
             { "Erosion iterations: more", "Erosion iterations", []( LandscapeSculptSettings& s )
               { s.Erosion.Iterations = std::min( s.Erosion.Iterations * 2, L::kLandscapeMaxErosionIterations ); },
               erosion } );
        controls.push_back( { "Erosion iterations: fewer", "Erosion iterations", []( LandscapeSculptSettings& s )
                              { s.Erosion.Iterations = std::max( s.Erosion.Iterations / 2, 1 ); }, erosion } );
        for ( const L::LandscapeErosionNoiseMode mode :
              { L::LandscapeErosionNoiseMode::Both, L::LandscapeErosionNoiseMode::Raise,
                L::LandscapeErosionNoiseMode::Lower } )
            controls.push_back( { std::string( "Erosion noise mode: " ) + LandscapeErosionNoiseModeName( mode ),
                                  "Erosion noise mode", [mode]( LandscapeSculptSettings& s )
                                  { s.Erosion.NoiseMode = mode; }, erosion } );
        controls.push_back(
             { "Erosion noise scale: larger", "Erosion noise scale", []( LandscapeSculptSettings& s )
               { s.Erosion.NoiseScale = std::min( s.Erosion.NoiseScale * 2.0f, L::kLandscapeMaxNoiseScale ); },
               erosion } );
        controls.push_back(
             { "Erosion noise scale: smaller", "Erosion noise scale", []( LandscapeSculptSettings& s )
               { s.Erosion.NoiseScale = std::max( s.Erosion.NoiseScale / 2.0f, L::kLandscapeMinNoiseScale ); },
               erosion } );

        controls.push_back( { "Rain amount: more", "Rain amount",
                              []( LandscapeSculptSettings& s ) {
                                  s.HydroErosion.RainAmount =
                                       std::min( s.HydroErosion.RainAmount * 2, L::kLandscapeMaxRainAmount );
                              },
                              hydro } );
        controls.push_back( { "Rain amount: less", "Rain amount", []( LandscapeSculptSettings& s )
                              { s.HydroErosion.RainAmount = std::max( s.HydroErosion.RainAmount / 2, 1 ); },
                              hydro } );
        controls.push_back(
             { "Sediment capacity: larger", "Sediment capacity", []( LandscapeSculptSettings& s )
               { s.HydroErosion.SedimentCapacity = std::min( s.HydroErosion.SedimentCapacity + 0.1f, 1.0f ); },
               hydro } );
        controls.push_back( { "Sediment capacity: smaller", "Sediment capacity",
                              []( LandscapeSculptSettings& s )
                              {
                                  s.HydroErosion.SedimentCapacity = std::max(
                                       s.HydroErosion.SedimentCapacity - 0.1f, L::kLandscapeMinSedimentCapacity );
                              },
                              hydro } );
        controls.push_back( { "Hydro iterations: more", "Hydro iterations",
                              []( LandscapeSculptSettings& s ) {
                                  s.HydroErosion.Iterations = std::min( s.HydroErosion.Iterations * 2,
                                                                        L::kLandscapeMaxErosionIterations );
                              },
                              hydro } );
        controls.push_back( { "Hydro iterations: fewer", "Hydro iterations", []( LandscapeSculptSettings& s )
                              { s.HydroErosion.Iterations = std::max( s.HydroErosion.Iterations / 2, 1 ); },
                              hydro } );
        for ( const L::LandscapeRainMode mode : { L::LandscapeRainMode::Both, L::LandscapeRainMode::Positive } )
            controls.push_back( { std::string( "Rain distribution: " ) + LandscapeRainModeName( mode ),
                                  "Rain distribution", [mode]( LandscapeSculptSettings& s )
                                  { s.HydroErosion.RainMode = mode; }, hydro } );
        controls.push_back( { "Rain scale: larger", "Rain scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.HydroErosion.RainScale =
                                       std::min( s.HydroErosion.RainScale * 2.0f, L::kLandscapeMaxNoiseScale );
                              },
                              hydro } );
        controls.push_back( { "Rain scale: smaller", "Rain scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.HydroErosion.RainScale =
                                       std::max( s.HydroErosion.RainScale / 2.0f, L::kLandscapeMinNoiseScale );
                              },
                              hydro } );
        controls.push_back( { "Hydro detail smooth: on", "Hydro detail smooth",
                              []( LandscapeSculptSettings& s ) { s.HydroErosion.DetailSmooth = true; }, hydro } );
        controls.push_back( { "Hydro detail smooth: off", "Hydro detail smooth",
                              []( LandscapeSculptSettings& s ) { s.HydroErosion.DetailSmooth = false; }, hydro } );
        controls.push_back( { "Hydro detail scale: larger", "Hydro detail scale",
                              []( LandscapeSculptSettings& s ) {
                                  s.HydroErosion.DetailScale = std::min( s.HydroErosion.DetailScale + 0.1f,
                                                                         L::kLandscapeMaxHydroDetailScale );
                              },
                              hydro } );
        controls.push_back(
             { "Hydro detail scale: smaller", "Hydro detail scale", []( LandscapeSculptSettings& s )
               { s.HydroErosion.DetailScale = std::max( s.HydroErosion.DetailScale - 0.1f, 0.0f ); }, hydro } );
        return controls;
    }
} // namespace Desert::Editor::Core

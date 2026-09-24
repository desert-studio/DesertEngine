#pragma once

#include <Engine/World/Landscape/LandscapePaint.hpp>
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
     * setting no unattended run can photograph. The palette offers exactly the rows of LandscapeToolControls();
     * the Landscape panel draws its tool strip and action buttons from those rows, and its sliders
     * (LandscapeToolProperties) edit only fields those rows step: a widget without a command cannot be written.
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
        Mirror,
        CopyPaste,
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
            case LandscapeTool::Mirror:
                return "Mirror";
            case LandscapeTool::CopyPaste:
                return "Copy/Paste";
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

    inline const char* LandscapeMirrorOpName( World::Landscape::LandscapeMirrorOp op )
    {
        using World::Landscape::LandscapeMirrorOp;
        switch ( op )
        {
            case LandscapeMirrorOp::MinusXToPlusX:
                return "-X to +X";
            case LandscapeMirrorOp::PlusXToMinusX:
                return "+X to -X";
            case LandscapeMirrorOp::MinusZToPlusZ:
                return "-Z to +Z";
            case LandscapeMirrorOp::PlusZToMinusZ:
                return "+Z to -Z";
            case LandscapeMirrorOp::RotateMinusXToPlusX:
                return "Rotate -X to +X";
            case LandscapeMirrorOp::RotatePlusXToMinusX:
                return "Rotate +X to -X";
            case LandscapeMirrorOp::RotateMinusZToPlusZ:
                return "Rotate -Z to +Z";
            case LandscapeMirrorOp::RotatePlusZToMinusZ:
                return "Rotate +Z to -Z";
        }
        return "Unknown";
    }

    inline const char* LandscapePasteModeName( World::Landscape::LandscapePasteMode mode )
    {
        switch ( mode )
        {
            case World::Landscape::LandscapePasteMode::Both:
                return "Both";
            case World::Landscape::LandscapePasteMode::Raise:
                return "Raise";
            case World::Landscape::LandscapePasteMode::Lower:
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
        LandscapeTool                                   Tool = LandscapeTool::Sculpt;
        World::Landscape::LandscapeBrushSettings        Brush;
        World::Landscape::LandscapeSmoothSettings       Smooth;
        World::Landscape::LandscapeFlattenSettings      Flatten;
        World::Landscape::LandscapeNoiseSettings        Noise;
        World::Landscape::LandscapeRampSettings         Ramp;
        World::Landscape::LandscapeErosionSettings      Erosion;
        World::Landscape::LandscapeHydroErosionSettings HydroErosion;
        World::Landscape::LandscapeMirrorSettings       Mirror;
        World::Landscape::LandscapePasteMode            PasteMode = World::Landscape::LandscapePasteMode::Both;
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
        MirrorPoint,
        MirrorApply,
        CopyCornerA,
        CopyCornerB,
        Copy,
        Paste,
    };

    /// UE's Landscape mode tabs: Paint is a MODE with its own tool, not one more sculpt tool, so the sculpt tool
    /// the user had picked survives a trip to Paint and back.
    enum class LandscapeEdMode : uint8_t
    {
        Sculpt,
        Paint,
    };

    struct LandscapeSculptState
    {
        LandscapeEdMode         Mode = LandscapeEdMode::Sculpt;
        LandscapeSculptSettings Settings;
        /// The Paint tool's settings; the brush (Settings.Brush) is shared with the sculpt tools, as in UE.
        World::Landscape::LandscapePaintSettings Paint;
        LandscapeStrokeRequest  Request = LandscapeStrokeRequest::None;
        /// UE's FLandscapeToolRamp::Points, in world cm; applying keeps them, as UE does until the tool is reset.
        std::optional<glm::vec3> RampStart;
        std::optional<glm::vec3> RampEnd;
        /// UE's MirrorPoint in world cm; unset means the landscape's centre (UE's CenterMirrorPoint).
        std::optional<glm::vec3> MirrorPoint;
        /// The copy region's corners (UE: the gizmo's extent) and what the last Copy took.
        std::optional<glm::vec3>              CopyCornerA;
        std::optional<glm::vec3>              CopyCornerB;
        World::Landscape::LandscapeCopyBuffer CopyBuffer;

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
                LandscapeTool::Erase, LandscapeTool::Ramp, LandscapeTool::Erosion, LandscapeTool::HydroErosion,
                LandscapeTool::Mirror, LandscapeTool::CopyPaste } )
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

        auto mirror = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Mirror; };
        auto paste  = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::CopyPaste; };
        using L::LandscapeMirrorOp;
        for ( const LandscapeMirrorOp op :
              { LandscapeMirrorOp::MinusXToPlusX, LandscapeMirrorOp::PlusXToMinusX,
                LandscapeMirrorOp::MinusZToPlusZ, LandscapeMirrorOp::PlusZToMinusZ,
                LandscapeMirrorOp::RotateMinusXToPlusX, LandscapeMirrorOp::RotatePlusXToMinusX,
                LandscapeMirrorOp::RotateMinusZToPlusZ, LandscapeMirrorOp::RotatePlusZToMinusZ } )
            controls.push_back( { std::string( "Mirror operation: " ) + LandscapeMirrorOpName( op ),
                                  "Mirror operation", [op]( LandscapeSculptSettings& s ) { s.Mirror.Op = op; },
                                  mirror } );
        controls.push_back( { "Mirror smoothing width: larger", "Mirror smoothing",
                              []( LandscapeSculptSettings& s ) {
                                  s.Mirror.SmoothingWidth =
                                       std::min( s.Mirror.SmoothingWidth + 2, L::kLandscapeMaxMirrorSmoothingUi );
                              },
                              mirror } );
        controls.push_back(
             { "Mirror smoothing width: smaller", "Mirror smoothing", []( LandscapeSculptSettings& s )
               { s.Mirror.SmoothingWidth = std::max( s.Mirror.SmoothingWidth - 2, 0 ); }, mirror } );
        controls.push_back( { "Mirror: set point at viewport centre",
                              "Mirror",
                              {},
                              mirror,
                              LandscapeStrokeRequest::MirrorPoint } );
        controls.push_back( { "Mirror: apply", "Mirror", {}, mirror, LandscapeStrokeRequest::MirrorApply } );

        using L::LandscapePasteMode;
        for ( const LandscapePasteMode mode :
              { LandscapePasteMode::Both, LandscapePasteMode::Raise, LandscapePasteMode::Lower } )
            controls.push_back( { std::string( "Paste mode: " ) + LandscapePasteModeName( mode ), "Paste mode",
                                  [mode]( LandscapeSculptSettings& s ) { s.PasteMode = mode; }, paste } );
        controls.push_back(
             { "Copy: set corner A at viewport centre", "Copy", {}, paste, LandscapeStrokeRequest::CopyCornerA } );
        controls.push_back(
             { "Copy: set corner B at viewport centre", "Copy", {}, paste, LandscapeStrokeRequest::CopyCornerB } );
        controls.push_back( { "Copy: copy region", "Copy", {}, paste, LandscapeStrokeRequest::Copy } );
        controls.push_back( { "Paste: at viewport centre", "Paste", {}, paste, LandscapeStrokeRequest::Paste } );
        return controls;
    }

    /**
     * @brief One row of the Landscape panel's Tool Settings section: UE's ULandscapeEditorObject property with
     *        its ShowForTools, UIMin/UIMax (the slider's travel) and ClampMin/ClampMax (what typing may reach).
     *
     * Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Public/LandscapeEditorObject.h:320-760, adapted: a
     * UPROPERTY's meta becomes this row, Slate's property grid becomes the panel's property rows. Each row edits a
     * field LandscapeToolControls() already steps from the palette, so a value the panel can set is still a value
     * an unattended run can set (see that table's WHY).
     */
    struct LandscapeToolProperty
    {
        enum class Kind : uint8_t
        {
            Float,
            Int,
            Bool,
            Choice,
        };

        const char*                                           Label = "";
        Kind                                                  Type  = Kind::Float;
        std::function<bool( const LandscapeSculptSettings& )> Shown;
        std::function<float*( LandscapeSculptSettings& )>     Float;
        std::function<int32_t*( LandscapeSculptSettings& )>   Int;
        std::function<bool*( LandscapeSculptSettings& )>      Bool;
        /// Choice rows: the option names, and the current option's index / setting it.
        std::vector<std::string>                             Options;
        std::function<int( LandscapeSculptSettings& )>       Chosen;
        std::function<void( LandscapeSculptSettings&, int )> Choose;
        float                                                UIMin    = 0.0f;
        float                                                UIMax    = 1.0f;
        float                                                ClampMin = 0.0f;
        float                                                ClampMax = 1.0f;
        /// UE's SliderExponent above 1: the slider spends its travel on the small end.
        bool Logarithmic = false;
    };

    template <typename E, typename Name, typename Field>
    LandscapeToolProperty MakeLandscapeChoice( const char*                                           label,
                                               std::function<bool( const LandscapeSculptSettings& )> shown,
                                               std::vector<E> values, Name name, Field field )
    {
        LandscapeToolProperty p;
        p.Label = label;
        p.Type  = LandscapeToolProperty::Kind::Choice;
        p.Shown = std::move( shown );
        for ( const E v : values )
            p.Options.emplace_back( name( v ) );
        p.Chosen = [values, field]( LandscapeSculptSettings& s )
        { return static_cast<int>( std::find( values.begin(), values.end(), field( s ) ) - values.begin() ); };
        p.Choose = [values, field]( LandscapeSculptSettings& s, int index )
        { field( s ) = values[static_cast<size_t>( index )]; };
        return p;
    }

    /// The brush falloff shapes in UE's order (ELandscapeBrushFalloffType: Smooth, Linear, Spherical, Tip).
    inline std::vector<World::Landscape::LandscapeBrushFalloff> LandscapeFalloffShapes()
    {
        using World::Landscape::LandscapeBrushFalloff;
        return { LandscapeBrushFalloff::Smooth, LandscapeBrushFalloff::Linear, LandscapeBrushFalloff::Spherical,
                 LandscapeBrushFalloff::Tip };
    }

    /// Whether the tool paints with the circle brush (UE: its ValidBrushes are not BrushSet_Dummy).
    inline bool LandscapeToolUsesBrush( LandscapeTool tool )
    {
        return tool != LandscapeTool::Ramp && tool != LandscapeTool::Mirror;
    }

    /// Tool Settings rows in UE's declaration order; ranges are UE's meta unless a named constant here is tighter.
    inline std::vector<LandscapeToolProperty> LandscapeToolProperties()
    {
        namespace L = World::Landscape;
        using S     = LandscapeSculptSettings;
        using Kind  = LandscapeToolProperty::Kind;
        using Shown = std::function<bool( const S& )>;
        auto is  = []( LandscapeTool tool ) -> Shown { return [tool]( const S& s ) { return s.Tool == tool; }; };
        auto flt = []( const char* label, Shown shown, std::function<float*( S& )> f, float uiMin, float uiMax,
                       float clampMin, float clampMax, bool log )
        {
            LandscapeToolProperty p;
            p.Label       = label;
            p.Type        = Kind::Float;
            p.Shown       = std::move( shown );
            p.Float       = std::move( f );
            p.UIMin       = uiMin;
            p.UIMax       = uiMax;
            p.ClampMin    = clampMin;
            p.ClampMax    = clampMax;
            p.Logarithmic = log;
            return p;
        };
        auto integer = []( const char* label, Shown shown, std::function<int32_t*( S& )> f, int32_t uiMin,
                           int32_t uiMax, int32_t clampMin, int32_t clampMax )
        {
            LandscapeToolProperty p;
            p.Label    = label;
            p.Type     = Kind::Int;
            p.Shown    = std::move( shown );
            p.Int      = std::move( f );
            p.UIMin    = static_cast<float>( uiMin );
            p.UIMax    = static_cast<float>( uiMax );
            p.ClampMin = static_cast<float>( clampMin );
            p.ClampMax = static_cast<float>( clampMax );
            return p;
        };
        auto boolean = []( const char* label, Shown shown, std::function<bool*( S& )> f )
        {
            LandscapeToolProperty p;
            p.Label = label;
            p.Type  = Kind::Bool;
            p.Shown = std::move( shown );
            p.Bool  = std::move( f );
            return p;
        };
        // UE's ToolStrength ShowForTools: every heightmap tool but Ramp and Mirror.
        const Shown strength = []( const S& s )
        { return s.Tool != LandscapeTool::Ramp && s.Tool != LandscapeTool::Mirror; };

        std::vector<LandscapeToolProperty> rows;
        rows.push_back( flt(
             "Tool Strength", strength, []( S& s ) { return &s.Brush.Strength; }, 0.0f, 1.0f,
             kLandscapeMinStrength, kLandscapeMaxStrength, false ) );
        // Flatten (LandscapeEditorObject.h:353-383).
        rows.push_back( MakeLandscapeChoice(
             "Flatten Mode", is( LandscapeTool::Flatten ),
             std::vector<L::LandscapeFlattenMode>{
                  L::LandscapeFlattenMode::Both, L::LandscapeFlattenMode::Raise, L::LandscapeFlattenMode::Lower,
                  L::LandscapeFlattenMode::Interval, L::LandscapeFlattenMode::Terrace },
             LandscapeFlattenModeName, []( S& s ) -> L::LandscapeFlattenMode& { return s.Flatten.Mode; } ) );
        rows.push_back( boolean( "Use Slope Flatten", is( LandscapeTool::Flatten ),
                                 []( S& s ) { return &s.Flatten.UseSlopeFlatten; } ) );
        rows.push_back( boolean( "Pick Value Per Apply", is( LandscapeTool::Flatten ),
                                 []( S& s ) { return &s.Flatten.PickValuePerApply; } ) );
        rows.push_back( flt(
             "Terrace Interval", is( LandscapeTool::Flatten ), []( S& s ) { return &s.Flatten.TerraceIntervalCm; },
             1.0f, 32768.0f, L::kLandscapeMinTerraceIntervalCm, L::kLandscapeMaxTerraceIntervalCm, true ) );
        rows.push_back( flt(
             "Terrace Smoothing", is( LandscapeTool::Flatten ), []( S& s ) { return &s.Flatten.TerraceSmooth; },
             0.0001f, 1.0f, L::kLandscapeMinTerraceSmooth, L::kLandscapeMaxTerraceSmooth, true ) );
        // Ramp (:398-403).
        rows.push_back( flt(
             "Ramp Width", is( LandscapeTool::Ramp ), []( S& s ) { return &s.Ramp.WidthCm; }, 1.0f,
             L::kLandscapeMaxRampWidthUiCm, L::kLandscapeMinRampWidthCm, kLandscapeMaxRadiusCm, true ) );
        rows.push_back( flt(
             "Side Falloff", is( LandscapeTool::Ramp ), []( S& s ) { return &s.Ramp.SideFalloff; }, 0.0f, 1.0f,
             0.0f, 1.0f, false ) );
        rows.push_back( MakeLandscapeChoice(
             "Ramp Mode", is( LandscapeTool::Ramp ),
             std::vector<L::LandscapeRampMode>{ L::LandscapeRampMode::Both, L::LandscapeRampMode::Raise,
                                                L::LandscapeRampMode::Lower },
             LandscapeRampModeName, []( S& s ) -> L::LandscapeRampMode& { return s.Ramp.Mode; } ) );
        // Smooth (:409-418).
        rows.push_back( integer(
             "Filter Kernel Radius", is( LandscapeTool::Smooth ),
             []( S& s ) { return &s.Smooth.FilterKernelRadius; }, 0, 7, L::kLandscapeSmoothMinRadius,
             L::kLandscapeSmoothMaxRadius ) );
        rows.push_back( boolean( "Detail Smooth", is( LandscapeTool::Smooth ),
                                 []( S& s ) { return &s.Smooth.DetailSmooth; } ) );
        rows.push_back( flt(
             "Detail Scale", is( LandscapeTool::Smooth ), []( S& s ) { return &s.Smooth.DetailScale; }, 0.0f,
             L::kLandscapeMaxDetailScale, 0.0f, L::kLandscapeMaxDetailScale, false ) );
        // Erosion (:423-444).
        rows.push_back( integer(
             "Threshold", is( LandscapeTool::Erosion ), []( S& s ) { return &s.Erosion.Threshold; }, 0, 128, 0,
             L::kLandscapeMaxErosionThreshold ) );
        rows.push_back( integer(
             "Iterations", is( LandscapeTool::Erosion ), []( S& s ) { return &s.Erosion.Iterations; }, 1, 150, 1,
             L::kLandscapeMaxErosionIterations ) );
        rows.push_back(
             MakeLandscapeChoice( "Noise Mode", is( LandscapeTool::Erosion ),
                                  std::vector<L::LandscapeErosionNoiseMode>{ L::LandscapeErosionNoiseMode::Both,
                                                                             L::LandscapeErosionNoiseMode::Raise,
                                                                             L::LandscapeErosionNoiseMode::Lower },
                                  LandscapeErosionNoiseModeName,
                                  []( S& s ) -> L::LandscapeErosionNoiseMode& { return s.Erosion.NoiseMode; } ) );
        rows.push_back( flt(
             "Noise Scale", is( LandscapeTool::Erosion ), []( S& s ) { return &s.Erosion.NoiseScale; }, 1.1f,
             256.0f, L::kLandscapeMinNoiseScale, L::kLandscapeMaxNoiseScale, false ) );
        // Hydro Erosion (:449-474).
        rows.push_back( integer(
             "Rain Amount", is( LandscapeTool::HydroErosion ), []( S& s ) { return &s.HydroErosion.RainAmount; },
             1, 256, 1, L::kLandscapeMaxRainAmount ) );
        rows.push_back( flt(
             "Sediment Capacity", is( LandscapeTool::HydroErosion ),
             []( S& s ) { return &s.HydroErosion.SedimentCapacity; }, L::kLandscapeMinSedimentCapacity, 1.0f,
             L::kLandscapeMinSedimentCapacity, 1.0f, false ) );
        rows.push_back( integer(
             "Iterations", is( LandscapeTool::HydroErosion ), []( S& s ) { return &s.HydroErosion.Iterations; }, 1,
             150, 1, L::kLandscapeMaxErosionIterations ) );
        rows.push_back( MakeLandscapeChoice(
             "Initial Rain Distribution", is( LandscapeTool::HydroErosion ),
             std::vector<L::LandscapeRainMode>{ L::LandscapeRainMode::Both, L::LandscapeRainMode::Positive },
             LandscapeRainModeName, []( S& s ) -> L::LandscapeRainMode& { return s.HydroErosion.RainMode; } ) );
        rows.push_back( flt(
             "Rain Distribution Scale", is( LandscapeTool::HydroErosion ),
             []( S& s ) { return &s.HydroErosion.RainScale; }, 1.1f, 256.0f, L::kLandscapeMinNoiseScale,
             L::kLandscapeMaxNoiseScale, false ) );
        rows.push_back( boolean( "Detail Smooth", is( LandscapeTool::HydroErosion ),
                                 []( S& s ) { return &s.HydroErosion.DetailSmooth; } ) );
        rows.push_back( flt(
             "Detail Scale", is( LandscapeTool::HydroErosion ), []( S& s ) { return &s.HydroErosion.DetailScale; },
             0.0f, L::kLandscapeMaxHydroDetailScale, 0.0f, L::kLandscapeMaxHydroDetailScale, false ) );
        // Noise (:479-484).
        rows.push_back( MakeLandscapeChoice(
             "Noise Mode", is( LandscapeTool::Noise ),
             std::vector<L::LandscapeNoiseMode>{ L::LandscapeNoiseMode::Both, L::LandscapeNoiseMode::Add,
                                                 L::LandscapeNoiseMode::Sub },
             LandscapeNoiseModeName, []( S& s ) -> L::LandscapeNoiseMode& { return s.Noise.Mode; } ) );
        rows.push_back( flt(
             "Noise Scale", is( LandscapeTool::Noise ), []( S& s ) { return &s.Noise.NoiseScale; }, 1.1f, 256.0f,
             L::kLandscapeMinNoiseScale, L::kLandscapeMaxNoiseScale, false ) );
        // Copy/Paste (:500-527).
        rows.push_back( MakeLandscapeChoice(
             "Paste Mode", is( LandscapeTool::CopyPaste ),
             std::vector<L::LandscapePasteMode>{ L::LandscapePasteMode::Both, L::LandscapePasteMode::Raise,
                                                 L::LandscapePasteMode::Lower },
             LandscapePasteModeName, []( S& s ) -> L::LandscapePasteMode& { return s.PasteMode; } ) );
        // Mirror (:531-540).
        rows.push_back( MakeLandscapeChoice(
             "Operation", is( LandscapeTool::Mirror ),
             std::vector<L::LandscapeMirrorOp>{
                  L::LandscapeMirrorOp::MinusXToPlusX, L::LandscapeMirrorOp::PlusXToMinusX,
                  L::LandscapeMirrorOp::MinusZToPlusZ, L::LandscapeMirrorOp::PlusZToMinusZ,
                  L::LandscapeMirrorOp::RotateMinusXToPlusX, L::LandscapeMirrorOp::RotatePlusXToMinusX,
                  L::LandscapeMirrorOp::RotateMinusZToPlusZ, L::LandscapeMirrorOp::RotatePlusZToMinusZ },
             LandscapeMirrorOpName, []( S& s ) -> L::LandscapeMirrorOp& { return s.Mirror.Op; } ) );
        rows.push_back( integer(
             "Smoothing Width", is( LandscapeTool::Mirror ), []( S& s ) { return &s.Mirror.SmoothingWidth; }, 0,
             L::kLandscapeMaxMirrorSmoothingUi, 0, L::kLandscapeMaxMirrorSmoothing ) );
        return rows;
    }
} // namespace Desert::Editor::Core

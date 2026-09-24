#pragma once

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    /**
     * @brief The Landscape mode's Sculpt / Smooth tool settings (UE's ULandscapeEditorObject subset) and the ONE
     *        table the tool panel draws its widgets from and the command palette offers as commands.
     *
     * WHY ONE TABLE. Synthetic mouse input is closed on this machine, so a setting only a hand can change is a
     * setting no unattended run can photograph. The panel draws NOTHING but the rows of LandscapeToolControls()
     * (plus read-outs), and the palette offers exactly those rows: a widget without a command cannot be written.
     */
    enum class LandscapeTool : uint8_t
    {
        Sculpt,
        Smooth,
    };

    inline const char* LandscapeToolName( LandscapeTool tool )
    {
        return tool == LandscapeTool::Sculpt ? "Sculpt" : "Smooth";
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
        LandscapeTool                             Tool = LandscapeTool::Sculpt;
        World::Landscape::LandscapeBrushSettings  Brush;
        World::Landscape::LandscapeSmoothSettings Smooth;
    };

    /// A stroke asked for from the palette: one step at the viewport centre, then the stroke ends.
    enum class LandscapeStrokeRequest : uint8_t
    {
        None,
        Raise,
        Lower,
    };

    struct LandscapeSculptState
    {
        LandscapeSculptSettings Settings;
        LandscapeStrokeRequest  Request = LandscapeStrokeRequest::None;

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
    };

    inline std::vector<LandscapeToolControl> LandscapeToolControls()
    {
        using World::Landscape::LandscapeBrushFalloff;
        auto always = []( const LandscapeSculptSettings& ) { return true; };
        auto smooth = []( const LandscapeSculptSettings& s ) { return s.Tool == LandscapeTool::Smooth; };

        std::vector<LandscapeToolControl> controls;
        for ( const LandscapeTool tool : { LandscapeTool::Sculpt, LandscapeTool::Smooth } )
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
        return controls;
    }
} // namespace Desert::Editor::Core

#pragma once

#include <Editor/Core/EditableProperty.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    /**
     * @brief THE MODELING PANEL'S DRAGGED VALUES, AS THE CONTROL CHANNEL'S `modeling` SUBJECT.
     *
     * A drag or slider in ModelingPanel has no name, so it is not a palette entry (ControlProtocol.hpp says
     * why: an entry per value is an argument list pretending to be a vocabulary). It is a row here instead,
     * and `set` writes the very ModelingState field the widget writes - one route into the value, two ways
     * to reach it. The panel's closed choices (axes, modes, on/off) are palette entries in
     * EditorLayer::BuildPaletteCommands; the census suite ModelingPaletteCensus holds both halves to the
     * panel's widget list.
     *
     * The clamps are the widget's own, so a value the panel cannot hold is refused rather than stored.
     */
    struct ModelingStateRow
    {
        const char* Name;  ///< what `set` names
        const char* Label; ///< the widget it stands for
        const char* Group;
        int         Components; ///< 1, or 3 for a float3
        bool        Integer;
        float       Min;
        float       Max;
        float* ( *Float )( ModelingState& ); ///< the field, when it is a float (or a float3's first component)
        int* ( *Int )( ModelingState& );     ///< the field, when it is an int
    };

    // clang-format off
    inline constexpr std::array<ModelingStateRow, 24> kModelingStateRows = { {
         { "CubeGrid.GridFrameOrigin",  "Grid Frame Origin",  "CubeGrid", 3, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.GridOrigin.x; }, nullptr },
         { "CubeGrid.CurrentBlockSize", "Current Block Size", "CubeGrid", 1, false, ModelingState::MinCellSize,
           ModelingState::MaxCellSize, []( ModelingState& s ) { return &s.CellSize; }, nullptr },
         { "CubeGrid.BlocksPerStep",    "Blocks / Step",      "CubeGrid", 1, true, 1.0f, 32.0f, nullptr,
           []( ModelingState& s ) { return &s.BlocksPerStep; } },
         { "CreateShape.Width",         "Width",              "Create Shape", 1, false, 1.0f, 100000.0f,
           []( ModelingState& s ) { return &s.CreateShape.Width; }, nullptr },
         { "CreateShape.Depth",         "Depth",              "Create Shape", 1, false, 1.0f, 100000.0f,
           []( ModelingState& s ) { return &s.CreateShape.Depth; }, nullptr },
         { "CreateShape.Height",        "Height",             "Create Shape", 1, false, 1.0f, 100000.0f,
           []( ModelingState& s ) { return &s.CreateShape.Height; }, nullptr },
         { "CreateShape.StepDepth",     "Step Depth",         "Create Shape", 1, false, 1.0f, 100000.0f,
           []( ModelingState& s ) { return &s.CreateShape.StepDepth; }, nullptr },
         { "CreateShape.StepHeight",    "Step Height",        "Create Shape", 1, false, 1.0f, 100000.0f,
           []( ModelingState& s ) { return &s.CreateShape.StepHeight; }, nullptr },
         { "CreateShape.Subdivisions",  "Subdivisions",       "Create Shape", 1, true, 1.0f, 32.0f, nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Subdivisions; } },
         { "CreateShape.Slices",        "Slices",             "Create Shape", 1, true, 3.0f, 128.0f, nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Slices; } },
         { "CreateShape.Stacks",        "Stacks",             "Create Shape", 1, true, 2.0f, 128.0f, nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Stacks; } },
         { "CreateShape.Steps",         "Steps",              "Create Shape", 1, true, 1.0f, 64.0f, nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Steps; } },
         { "Element.Distance",          "Distance",           "Select Elements", 1, false, -10000.0f, 10000.0f,
           []( ModelingState& s ) { return &s.ElementOpDistance; }, nullptr },
         { "Element.LoopPosition",      "Loop at",            "Select Elements", 1, false, 0.01f, 0.99f,
           []( ModelingState& s ) { return &s.ElementLoopPosition; }, nullptr },
         { "Element.WeldTolerance",     "Weld",               "Select Elements", 1, false, 0.0f, 10.0f,
           []( ModelingState& s ) { return &s.ElementWeldTolerance; }, nullptr },
         { "Element.SubdivideLevels",   "Levels",             "Select Elements", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxSubdivideLevels ), nullptr,
           []( ModelingState& s ) { return &s.ElementSubdivideLevels; } },
         { "PlaneCut.Offset",           "Offset",             "Plane Cut", 1, false, -100000.0f, 100000.0f,
           []( ModelingState& s ) { return &s.ElementPlaneCutOffset; }, nullptr },
         { "XForm.PivotWorldPoint",     "World Point",        "XForm", 3, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPivotWorldPoint.x; }, nullptr },
         { "Pattern.Count",             "Count",              "Pattern", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxPatternCopies ), nullptr,
           []( ModelingState& s ) { return &s.XformPattern.Count; } },
         { "Pattern.Radius",            "Radius",             "Pattern", 1, false, 0.0f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.Radius; }, nullptr },
         { "Pattern.Spacing",           "Spacing",            "Pattern", 1, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.Spacing; }, nullptr },
         { "Pattern.CountB",            "Count B",            "Pattern", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxPatternCopies ), nullptr,
           []( ModelingState& s ) { return &s.XformPattern.CountB; } },
         { "Pattern.SpacingB",          "Spacing B",          "Pattern", 1, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.SpacingB; }, nullptr },
         { "Pattern.SweepDegrees",      "Sweep",              "Pattern", 1, false, -360.0f, 360.0f,
           []( ModelingState& s ) { return &s.XformPattern.SweepDegrees; }, nullptr },
    } };
    // clang-format on

    [[nodiscard]] inline std::vector<EditableProperty> DescribeModelingState( ModelingState& state )
    {
        std::vector<EditableProperty> census;
        for ( const ModelingStateRow& row : kModelingStateRows )
        {
            EditableProperty property;
            property.Name       = row.Name;
            property.Label      = row.Label;
            property.Group      = row.Group;
            property.Type       = row.Integer ? "int" : ( row.Components == 3 ? "float3" : "float" );
            property.Components = row.Components;
            property.Min        = row.Min;
            property.Max        = row.Max;
            if ( row.Integer )
                property.Value[0] = static_cast<float>( *row.Int( state ) );
            else
                for ( int i = 0; i < row.Components; ++i )
                    property.Value[static_cast<std::size_t>( i )] = row.Float( state )[i];
            census.push_back( property );
        }
        return census;
    }

    [[nodiscard]] inline Common::BoolResultStr
    SetModelingStateProperty( ModelingState& state, const std::string& property, const std::vector<float>& value )
    {
        for ( const ModelingStateRow& row : kModelingStateRows )
        {
            if ( property != row.Name )
                continue;
            if ( static_cast<int>( value.size() ) != row.Components )
                return Common::MakeFormattedError<bool>( "'{}' takes {} number(s) and was given {}", property,
                                                         row.Components, value.size() );
            for ( const float v : value )
                if ( !std::isfinite( v ) || v < row.Min || v > row.Max )
                    return Common::MakeFormattedError<bool>(
                         "'{}' = {} is outside the panel's range [{}, {}]; the widget cannot hold it either",
                         property, v, row.Min, row.Max );
            if ( row.Integer )
            {
                if ( value[0] != std::floor( value[0] ) )
                    return Common::MakeFormattedError<bool>( "'{}' is a whole number and was given {}", property,
                                                             value[0] );
                *row.Int( state ) = static_cast<int>( value[0] );
            }
            else
                for ( int i = 0; i < row.Components; ++i )
                    row.Float( state )[i] = value[static_cast<std::size_t>( i )];
            return Common::MakeSuccess( true );
        }

        std::string known;
        for ( const ModelingStateRow& row : kModelingStateRows )
            known += ( known.empty() ? "" : ", " ) + std::string( row.Name );
        return Common::MakeFormattedError<bool>( "'{}' is not a Modeling panel value. The subject offers: {}",
                                                 property, known );
    }
} // namespace Desert::Editor::Core

#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Editor/Core/Selection/FoliagePaint.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace Desert::Editor::Core
{
    /**
     * @brief THE FOLIAGE TOOL'S SETTINGS AS ROWS: the control channel's subject 'tool' while Foliage mode is
     *        on (UE Remote Control reaching FFoliageUISettings by property name).
     *
     * The Foliage panel's Brush Options widgets bind the same FoliagePaint fields and take their ranges from
     * the constants below, so `set tool BrushSize 3000` lands where the slider writes and is refused where
     * the slider cannot go. Values travel as TEXT, the form the reflected `set` already uses (a number, a
     * bool, a tool name), so one desertctl form covers entities, the world and the active tool.
     */
    inline constexpr float kFoliageBrushSizeMin = 50.0f; // cm, radius
    inline constexpr float kFoliageBrushSizeMax = 6000.0f;

    struct FoliageToolRow
    {
        enum class Kind : uint8_t
        {
            Float,
            Bool,
            Tool,
        };

        std::string_view Name;  ///< what `set tool` names
        std::string_view Label; ///< the panel widget it stands for
        Kind             Type = Kind::Float;
        float            Min  = 0.0f;
        float            Max  = 1.0f;
        float* ( *Float )()   = nullptr;
        bool* ( *Bool )()     = nullptr;
    };

    // clang-format off
    inline constexpr std::array<FoliageToolRow, 6> kFoliageToolRows = { {
         { "Tool", "Tool", FoliageToolRow::Kind::Tool, 0.0f, 0.0f, nullptr, nullptr },
         { "BrushSize", "Brush Size", FoliageToolRow::Kind::Float, kFoliageBrushSizeMin, kFoliageBrushSizeMax,
           [] { return &FoliagePaint::BrushRadius(); }, nullptr },
         { "PaintDensity", "Paint Density", FoliageToolRow::Kind::Float, 0.0f, 1.0f,
           [] { return &FoliagePaint::PaintDensity(); }, nullptr },
         { "FilterLandscape", "Landscape", FoliageToolRow::Kind::Bool, 0.0f, 0.0f, nullptr,
           [] { return &FoliagePaint::FilterLandscape(); } },
         { "FilterStaticMesh", "Static Mesh", FoliageToolRow::Kind::Bool, 0.0f, 0.0f, nullptr,
           [] { return &FoliagePaint::FilterStaticMesh(); } },
         { "BrushLayerMinWeight", "Layer Min Weight", FoliageToolRow::Kind::Float, 0.0f, 1.0f,
           [] { return &FoliagePaint::BrushLayerMinWeight(); }, nullptr },
    } };
    // clang-format on

    inline constexpr FoliageTool kFoliageTools[] = { FoliageTool::Paint, FoliageTool::Single, FoliageTool::Select,
                                                     FoliageTool::Lasso, FoliageTool::Remove, FoliageTool::Reapply,
                                                     FoliageTool::Fill };

    [[nodiscard]] inline const char* FoliageToolRowType( const FoliageToolRow& row )
    {
        switch ( row.Type )
        {
            case FoliageToolRow::Kind::Float:
                return "float";
            case FoliageToolRow::Kind::Bool:
                return "bool";
            case FoliageToolRow::Kind::Tool:
                return "enum";
        }
        return "?";
    }

    /// The row's current value in the form SetFoliageToolProperty parses back.
    [[nodiscard]] inline std::string FoliageToolRowText( const FoliageToolRow& row )
    {
        switch ( row.Type )
        {
            case FoliageToolRow::Kind::Float:
                return std::format( "{}", *row.Float() );
            case FoliageToolRow::Kind::Bool:
                return *row.Bool() ? "true" : "false";
            case FoliageToolRow::Kind::Tool:
                return FoliageToolName( FoliagePaint::Tool() );
        }
        return {};
    }

    [[nodiscard]] inline Common::BoolResultStr SetFoliageToolProperty( std::string_view name,
                                                                       std::string_view text )
    {
        for ( const FoliageToolRow& row : kFoliageToolRows )
        {
            if ( name != row.Name )
                continue;
            switch ( row.Type )
            {
                case FoliageToolRow::Kind::Float:
                {
                    const std::string digits( text );
                    char*             end = nullptr;
                    const float       v   = digits.empty() ? NAN : std::strtof( digits.c_str(), &end );
                    if ( digits.empty() || end != digits.c_str() + digits.size() || !std::isfinite( v ) )
                        return Common::MakeFormattedError<bool>( "'{}' takes a number and was given '{}'", name,
                                                                 text );
                    if ( v < row.Min || v > row.Max )
                        return Common::MakeFormattedError<bool>(
                             "'{}' = {} is outside the panel's range [{}, {}]; the widget cannot hold it either",
                             name, v, row.Min, row.Max );
                    *row.Float() = v;
                    return Common::MakeSuccess( true );
                }
                case FoliageToolRow::Kind::Bool:
                {
                    if ( text != "true" && text != "false" )
                        return Common::MakeFormattedError<bool>( "'{}' takes true or false and was given '{}'",
                                                                 name, text );
                    *row.Bool() = ( text == "true" );
                    return Common::MakeSuccess( true );
                }
                case FoliageToolRow::Kind::Tool:
                {
                    std::string known;
                    for ( const FoliageTool tool : kFoliageTools )
                    {
                        if ( text == FoliageToolName( tool ) )
                        {
                            FoliagePaint::Tool() = tool;
                            return Common::MakeSuccess( true );
                        }
                        known += ( known.empty() ? "" : ", " ) + std::string( FoliageToolName( tool ) );
                    }
                    return Common::MakeFormattedError<bool>( "'{}' is not a foliage tool; the tools are {}", text,
                                                             known );
                }
            }
        }

        std::string known;
        for ( const FoliageToolRow& row : kFoliageToolRows )
            known += ( known.empty() ? "" : ", " ) + std::string( row.Name );
        return Common::MakeFormattedError<bool>( "'{}' is not a Foliage tool setting. The tool offers: {}", name,
                                                 known );
    }
} // namespace Desert::Editor::Core

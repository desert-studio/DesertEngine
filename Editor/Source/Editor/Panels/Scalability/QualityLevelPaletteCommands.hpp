#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Settings/Scalability.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // The Scalability panel's per-group level combos as palette commands ("Quality: Shadows Cinematic"), so the
    // control channel (DesertCtl `run Quality "Quality: Shadows Cinematic"`) and the keyboard reach the SAME apply
    // point the combo uses: QualityState::SetGroupLevel, which drops the group's overrides (UE), resolves, logs a
    // fallback once and saves machine.json. One entry per group and level.
    [[nodiscard]] inline std::vector<PaletteCommand> QualityLevelPaletteCommands()
    {
        using Common::Scalability::Group;
        using Common::Scalability::Level;
        using Common::Scalability::QualityState;

        std::vector<PaletteCommand> commands;
        commands.reserve( Common::Scalability::kGroupCount * Common::Scalability::kLevelCount );
        for ( std::size_t g = 0; g < Common::Scalability::kGroupCount; ++g )
            for ( std::size_t l = 0; l < Common::Scalability::kLevelCount; ++l )
            {
                const auto group = static_cast<Group>( g );
                const auto level = static_cast<Level>( l );
                commands.push_back(
                     { "Quality",
                       std::format( "Quality: {} {}", rfl::enum_to_string( group ), rfl::enum_to_string( level ) ),
                       [group, level]() -> Common::BoolResultStr
                       {
                           const auto report = QualityState::SetGroupLevel( group, level );
                           if ( !report.Refused.empty() )
                               return Common::MakeError( std::string( report.Refused ) );
                           if ( !report.Kept() )
                               return Common::MakeError(
                                    "the quality level applies but machine.json was not saved" );
                           return PaletteCommandDone();
                       } } );
            }
        return commands;
    }

    // EVERY listed Scalability parameter as an explicit override ("Quality: PathTracing.Denoiser OIDN",
    // "Quality: PathTracing.SamplesPerPixel 1024"), UE's console variables (r.* set after sg.* applied): one
    // generic path, QualityState::SetOverride, so it REPLACES the group level's value and is saved to
    // machine.json. GENERATED FROM THE TABLE (ParameterSpecs), never listed by hand, so a new row is reachable the
    // day it gains a reader. A by-name parameter offers each of its names (ParameterValueNames, index = value); a
    // numeric one offers its values: every value when the range is at most kEveryValueSpan wide, otherwise Min,
    // Max, the spec Default, the value every level of the loaded table gives it, and the powers of two inside
    // the range. "... Level" clears the override, so the group's level applies again. Resolution.Percent is left
    // to QualityRenderScalePaletteCommands (its entries follow the device's RenderScale range).
    [[nodiscard]] inline std::vector<Common::Scalability::ParameterValue>
    QualityOverridePaletteValues( const Common::Scalability::ParameterSpec&    spec,
                                  const Common::Scalability::ScalabilityTable& table )
    {
        using Common::Scalability::Level;
        using Common::Scalability::ParameterValue;

        constexpr int64_t           kEveryValueSpan = 64;
        std::vector<ParameterValue> values;
        if ( static_cast<int64_t>( spec.Max ) - static_cast<int64_t>( spec.Min ) <= kEveryValueSpan )
        {
            for ( ParameterValue v = spec.Min; v <= spec.Max; ++v )
                values.push_back( v );
            return values;
        }
        values.push_back( spec.Min );
        values.push_back( spec.Max );
        values.push_back( spec.Default );
        for ( std::size_t l = 0; l < Common::Scalability::kLevelCount; ++l )
            values.push_back( table.ValueAt( spec.Id, static_cast<Level>( l ) ) );
        for ( int64_t p = 1; p <= static_cast<int64_t>( spec.Max ); p *= 2 )
            if ( p >= static_cast<int64_t>( spec.Min ) )
                values.push_back( static_cast<ParameterValue>( p ) );
        std::sort( values.begin(), values.end() );
        values.erase( std::unique( values.begin(), values.end() ), values.end() );
        return values;
    }

    [[nodiscard]] inline std::vector<PaletteCommand> QualityOverridePaletteCommands()
    {
        using Common::Scalability::Parameter;
        using Common::Scalability::ParameterSpec;
        using Common::Scalability::ParameterValue;
        using Common::Scalability::QualityState;

        std::vector<PaletteCommand> commands;
        const auto set = [&commands]( const Parameter parameter, const std::string_view key,
                                      const std::string_view valueLabel, const ParameterValue value )
        {
            commands.push_back( { "Quality", std::format( "Quality: {} {}", key, valueLabel ),
                                  [parameter, value]() -> Common::BoolResultStr
                                  {
                                      const auto report = QualityState::SetOverride( parameter, value );
                                      if ( !report.Refused.empty() )
                                          return Common::MakeError( std::string( report.Refused ) );
                                      if ( !report.Kept() )
                                          return Common::MakeError(
                                               "the override applies but machine.json was not saved" );
                                      return PaletteCommandDone();
                                  } } );
        };
        for ( const Parameter parameter : Common::Scalability::ListedParameters() )
        {
            if ( parameter == Parameter::RenderScalePercent )
                continue;
            const ParameterSpec&                    spec  = Common::Scalability::SpecOf( parameter );
            const std::span<const std::string_view> names = Common::Scalability::ParameterValueNames( parameter );
            if ( !names.empty() )
                for ( std::size_t v = 0; v < names.size(); ++v )
                    set( parameter, spec.Key, names[v], static_cast<ParameterValue>( v ) );
            else
                for ( const ParameterValue value : QualityOverridePaletteValues( spec, QualityState::Table() ) )
                    set( parameter, spec.Key, std::to_string( value ), value );
            commands.push_back(
                 { "Quality", std::format( "Quality: {} Level", spec.Key ), [parameter]() -> Common::BoolResultStr
                   {
                       const auto report = QualityState::ClearOverride( parameter );
                       if ( !report.Refused.empty() )
                           return Common::MakeError( std::string( report.Refused ) );
                       if ( !report.Kept() )
                           return Common::MakeError( "the level applies but machine.json was not saved" );
                       return PaletteCommandDone();
                   } } );
        }
        return commands;
    }

    // The render scale as an explicit override ("Quality: Resolution.Percent 67"), UE's r.ScreenPercentage: the
    // percent every temporal method reconstructs from (Scalability UpscalerForScale), set through the same apply
    // point as the Scalability panel's slider (QualityState::SetOverride, saved to machine.json). The entries are
    // the menu modes' percents (DLSS/FSR Ultra Performance 33, Performance 50, Balanced 58, Quality 67, Ultra
    // Quality 77), native, and supersampled 150 / 200, within the device's RenderScale range; "... Level" clears
    // the override, so the ResolutionScale level's percent applies again.
    [[nodiscard]] inline std::vector<PaletteCommand>
    QualityRenderScalePaletteCommands( const Common::Scalability::CapabilityCatalog& catalog )
    {
        using Common::Scalability::Parameter;
        using Common::Scalability::QualityState;

        const std::string_view      key = Common::Scalability::SpecOf( Parameter::RenderScalePercent ).Key;
        std::vector<PaletteCommand> commands;
        for ( const int percent : { 33, 50, 58, 67, 77, 100, 150, 200 } )
        {
            if ( percent < catalog.RenderScale.MinPercent || percent > catalog.RenderScale.MaxPercent )
                continue;
            commands.push_back(
                 { "Quality", std::format( "Quality: {} {}", key, percent ), [percent]() -> Common::BoolResultStr
                   {
                       const auto report = QualityState::SetOverride( Parameter::RenderScalePercent, percent );
                       if ( !report.Refused.empty() )
                           return Common::MakeError( std::string( report.Refused ) );
                       if ( !report.Kept() )
                           return Common::MakeError( "the render scale applies but machine.json was not saved" );
                       return PaletteCommandDone();
                   } } );
        }
        commands.push_back( { "Quality", std::format( "Quality: {} Level", key ), []() -> Common::BoolResultStr
                              {
                                  const auto report = QualityState::ClearOverride( Parameter::RenderScalePercent );
                                  if ( !report.Refused.empty() )
                                      return Common::MakeError( std::string( report.Refused ) );
                                  if ( !report.Kept() )
                                      return Common::MakeError(
                                           "the level applies but machine.json was not saved" );
                                  return PaletteCommandDone();
                              } } );
        return commands;
    }
} // namespace Desert::Editor

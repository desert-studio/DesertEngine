#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Settings/Scalability.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <format>
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
                           if ( !report.Saved )
                               return Common::MakeError(
                                    "the quality level applies but machine.json was not saved" );
                           return PaletteCommandDone();
                       } } );
            }
        return commands;
    }

    // The three render methods as explicit overrides ("Quality: GlobalIllumination.Method SoftwareTraced"), the
    // console variable of UE (r.DynamicGlobalIlluminationMethod & co.): QualityState::SetOverride, so it REPLACES
    // the view's PostProcessSettings request (ResolveRenderMethods, MethodLimit::Override) and is saved to
    // machine.json. "... Level" clears it: the group's level is a ceiling again. One entry per value plus Level.
    [[nodiscard]] inline std::vector<PaletteCommand> QualityMethodOverridePaletteCommands()
    {
        using Common::Scalability::Parameter;
        using Common::Scalability::ParameterValue;
        using Common::Scalability::QualityState;

        std::vector<PaletteCommand> commands;
        const auto                  append = [&commands]( const Parameter parameter, const auto& names )
        {
            const std::string_view key = Common::Scalability::SpecOf( parameter ).Key;
            for ( std::size_t v = 0; v < names.size(); ++v )
                commands.push_back( { "Quality", std::format( "Quality: {} {}", key, names[v] ),
                                      [parameter, v]() -> Common::BoolResultStr
                                      {
                                          const auto report = QualityState::SetOverride(
                                               parameter, static_cast<ParameterValue>( v ) );
                                          if ( !report.Refused.empty() )
                                              return Common::MakeError( std::string( report.Refused ) );
                                          if ( !report.Saved )
                                              return Common::MakeError(
                                                   "the method override applies but machine.json was not saved" );
                                          return PaletteCommandDone();
                                      } } );
            commands.push_back(
                 { "Quality", std::format( "Quality: {} Level", key ), [parameter]() -> Common::BoolResultStr
                   {
                       const auto report = QualityState::ClearOverride( parameter );
                       if ( !report.Refused.empty() )
                           return Common::MakeError( std::string( report.Refused ) );
                       if ( !report.Saved )
                           return Common::MakeError( "the level applies but machine.json was not saved" );
                       return PaletteCommandDone();
                   } } );
        };
        append( Parameter::GlobalIlluminationMethod, Common::Scalability::kGIMethodNames );
        append( Parameter::ReflectionsMethod, Common::Scalability::kReflectionMethodNames );
        append( Parameter::ShadowsMethod, Common::Scalability::kShadowMethodNames );
        return commands;
    }
} // namespace Desert::Editor

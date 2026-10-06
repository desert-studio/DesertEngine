#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Settings/Scalability.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <format>
#include <vector>

namespace Desert::Editor
{
    // The Scalability panel's anti-aliasing choice as palette commands, so the control channel (DesertCtl
    // `run`) and the keyboard reach the SAME apply point the combo uses: QualityState::SetOverride, which
    // resolves, logs a fallback once and saves machine.json. One entry per value the device's CapabilityCatalog
    // offers: None, FXAA, SMAA and MSAA at each catalog sample count > 1. The temporal methods are left out until
    // a pass runs them (TAA1), as in the panel.
    //
    // MSAA entries stay in the list in every scene (AA2): they STORE the machine's choice, which applies in
    // forward scenes; a deferred scene runs FXAA (Scalability::ResolveAntiAliasingForPath).
    [[nodiscard]] inline std::vector<PaletteCommand>
    AntiAliasingPaletteCommands( const Common::Scalability::CapabilityCatalog& catalog )
    {
        using Common::Scalability::AntiAliasingMethod;
        using Common::Scalability::Parameter;
        using Common::Scalability::ParameterValue;
        using Common::Scalability::QualityState;

        const auto choose = []( const AntiAliasingMethod method, const int samples )
        {
            return [method, samples]() -> Common::BoolResultStr
            {
                const auto methodReport = QualityState::SetOverride( Parameter::AntiAliasingMethod,
                                                                     static_cast<ParameterValue>( method ) );
                if ( !methodReport.Refused.empty() )
                    return Common::MakeError( std::string( methodReport.Refused ) );
                if ( samples > 1 )
                {
                    const auto samplesReport =
                         QualityState::SetOverride( Parameter::AntiAliasingSamples, samples );
                    if ( !samplesReport.Refused.empty() )
                        return Common::MakeError( std::string( samplesReport.Refused ) );
                    if ( !samplesReport.Saved )
                        return Common::MakeError(
                             "the anti-aliasing method applies but machine.json was not saved" );
                }
                else if ( !methodReport.Saved )
                    return Common::MakeError( "the anti-aliasing method applies but machine.json was not saved" );
                return PaletteCommandDone();
            };
        };

        std::vector<PaletteCommand> commands;
        for ( const AntiAliasingMethod method : catalog.AntiAliasingMethods )
        {
            if ( method == AntiAliasingMethod::None || method == AntiAliasingMethod::FXAA ||
                 method == AntiAliasingMethod::SMAA )
                commands.push_back( { "Anti-Aliasing",
                                      std::format( "Anti-Aliasing: {}", rfl::enum_to_string( method ) ),
                                      choose( method, 0 ) } );
            else if ( method == AntiAliasingMethod::MSAA )
                for ( const int samples : catalog.MSAACounts )
                    if ( samples > 1 )
                        commands.push_back( { "Anti-Aliasing", std::format( "Anti-Aliasing: MSAA {}x", samples ),
                                              choose( AntiAliasingMethod::MSAA, samples ) } );
        }
        return commands;
    }
} // namespace Desert::Editor

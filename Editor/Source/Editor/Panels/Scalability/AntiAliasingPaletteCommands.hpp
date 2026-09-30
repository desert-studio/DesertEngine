#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <format>
#include <vector>

namespace Desert::Editor
{
    // The Scalability panel's anti-aliasing choice as palette commands, so the control channel (DesertCtl
    // `run`) and the keyboard reach the SAME store the combo writes: MachineSettings, saved to machine.json,
    // applied by SceneRenderer on the next frame. One entry per value a person can pick: None, FXAA, SMAA
    // and MSAA at each count this device runs.
    [[nodiscard]] inline std::vector<PaletteCommand> AntiAliasingPaletteCommands( const int maxMsaaSamples )
    {
        using Common::Settings::AntiAliasingMethod;
        using Common::Settings::MachineSettings;

        const auto choose = []( const AntiAliasingMethod method, const int samples )
        {
            return [method, samples]() -> Common::BoolResultStr
            {
                MachineSettings& quality = MachineSettings::Get();
                quality.AAMethod         = method;
                if ( samples > 1 )
                    quality.MSAASamples = samples;
                if ( !MachineSettings::Save() )
                    return Common::MakeError( "the anti-aliasing method applies but machine.json was not saved" );
                return PaletteCommandDone();
            };
        };

        std::vector<PaletteCommand> commands;
        commands.push_back( { "Anti-Aliasing", "Anti-Aliasing: None", choose( AntiAliasingMethod::None, 0 ) } );
        commands.push_back( { "Anti-Aliasing", "Anti-Aliasing: FXAA", choose( AntiAliasingMethod::FXAA, 0 ) } );
        commands.push_back( { "Anti-Aliasing", "Anti-Aliasing: SMAA", choose( AntiAliasingMethod::SMAA, 0 ) } );
        for ( const int samples : { 2, 4, 8 } )
            if ( samples <= maxMsaaSamples )
                commands.push_back( { "Anti-Aliasing", std::format( "Anti-Aliasing: MSAA {}x", samples ),
                                      choose( AntiAliasingMethod::MSAA, samples ) } );
        return commands;
    }
} // namespace Desert::Editor

#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Settings/MachineSettings.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <format>
#include <functional>
#include <vector>

namespace Desert::Editor
{
    // The Scalability panel's anti-aliasing choice as palette commands, so the control channel (DesertCtl
    // `run`) and the keyboard reach the SAME store the combo writes: MachineSettings, saved to machine.json,
    // applied by SceneRenderer on the next frame. One entry per value a person can pick: None, FXAA, SMAA
    // and MSAA at each count this device runs.
    //
    // MSAA entries stay in the list in every scene (AA2): they STORE the machine's choice, which applies in
    // forward scenes. What the command reports is the EFFECTIVE result for the active scene's path
    // (`activeSceneIsForward`, MachineSettings::EffectiveAA), so picking MSAA in a deferred scene says that
    // the frame runs FXAA instead of pretending it applied.
    [[nodiscard]] inline std::vector<PaletteCommand>
    AntiAliasingPaletteCommands( const int maxMsaaSamples, std::function<bool()> activeSceneIsForward )
    {
        using Common::Settings::AntiAliasingMethod;
        using Common::Settings::MachineSettings;

        const auto choose = [activeSceneIsForward]( const AntiAliasingMethod method, const int samples )
        {
            return [method, samples, activeSceneIsForward]() -> Common::BoolResultStr
            {
                MachineSettings& quality = MachineSettings::Get();
                quality.AAMethod         = method;
                if ( samples > 1 )
                    quality.MSAASamples = samples;
                if ( !MachineSettings::Save() )
                    return Common::MakeError( "the anti-aliasing method applies but machine.json was not saved" );
                const bool forward   = activeSceneIsForward();
                const auto effective = quality.EffectiveAA( forward );
                LOG_INFO( "[Anti-Aliasing] chosen {}{}; effective in this {} scene: {}, {} sample(s){}",
                          rfl::enum_to_string( method ),
                          method == AntiAliasingMethod::MSAA ? std::format( " {}x", quality.MSAASamples ) : "",
                          forward ? "forward" : "deferred", rfl::enum_to_string( effective.Method ),
                          effective.Samples,
                          effective.MSAAUnavailableOnPath ? " (MSAA applies to forward scenes only)" : "" );
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

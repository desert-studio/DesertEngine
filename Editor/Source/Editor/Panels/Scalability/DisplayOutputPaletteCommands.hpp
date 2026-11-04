#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <rflcpp/rfl/enums.hpp>

#include <format>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // HDR-OUTPUT: the ONE apply point of machine.json Display (UE r.HDR.EnableHDROutput / r.HDR.Display.*): saves
    // machine.json and rebuilds the swapchain with it (Window::SetDisplay), which resolves the output against the
    // catalog and logs what it built. The Scalability panel's Display section and the palette both call it.
    [[nodiscard]] inline bool ApplyMachineDisplay()
    {
        const bool saved = Common::Settings::MachineSettings::Save();
        EngineContext::GetInstance().GetWindow()->SetDisplay( Common::Settings::MachineSettings::Get().Display );
        return saved;
    }

    // The display outputs as palette commands ("Display: Output scRGB_Linear"), one per catalog entry — the
    // control channel (DesertCtl `run Display "Display: Output scRGB_Linear"`) reaches the panel's combo.
    [[nodiscard]] inline std::vector<PaletteCommand> DisplayOutputPaletteCommands()
    {
        std::vector<PaletteCommand> commands;
        for ( const auto output : EngineContext::GetInstance().GetCapabilities().Catalog.DisplayOutputs )
            commands.push_back( { "Display", std::format( "Display: Output {}", rfl::enum_to_string( output ) ),
                                  [output]() -> Common::BoolResultStr
                                  {
                                      Common::Settings::MachineSettings::Get().Display.Output = output;
                                      if ( !ApplyMachineDisplay() )
                                          return Common::MakeError(
                                               "the display output applies but machine.json was not saved" );
                                      return PaletteCommandDone();
                                  } } );
        // The output transform's peak (HDR-OUTPUT p4; UE r.HDR.Display.OutputDevice's ACES 1000 / 2000 nits
        // devices, r.HDR.Display.MaxLuminance): "Display: Peak 1000 nits" sets machine.json Display.PeakNits,
        // "Display: Peak OS" returns it to 0 (the OS's figure). The panel's "Peak Override" field is the same value.
        for ( const float nits : { 0.0f, 1000.0f, 2000.0f } )
            commands.push_back( { "Display",
                                  nits > 0.0f ? std::format( "Display: Peak {:.0f} nits", nits )
                                              : std::string( "Display: Peak OS" ),
                                  [nits]() -> Common::BoolResultStr
                                  {
                                      Common::Settings::MachineSettings::Get().Display.PeakNits = nits;
                                      if ( !ApplyMachineDisplay() )
                                          return Common::MakeError(
                                               "the display peak applies but machine.json was not saved" );
                                      return PaletteCommandDone();
                                  } } );
        return commands;
    }
} // namespace Desert::Editor

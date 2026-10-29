#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Neural/FusedMlpCompute.hpp>

#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // THE NN LAYER'S FORCED KERNEL PATH as palette commands (NN-RUNTIME step 3; UE would keep it as a console
    // variable): "Neural: Kernel path Auto" clears machine.json ForcedNeuralKernelPath (the device's selection),
    // "... Fp32" / "Fp16" / "CoopMatFp16" force that path. Like a UE cvar, a path the device cannot run is still
    // SET (with a warning naming what it lacks): every consumer asks Neural::ResolveNeuralKernelPath and refuses
    // it with that reason (nn-check reports it "refused"). An unattended run keeps it in the process
    // (MachineSettings:: KeepInSession), so a scenario never leaves it in machine.json. The control channel
    // reaches them as `run Neural "Neural: Kernel path Fp16"`; DesertCtl nn-check then runs the forced path.
    [[nodiscard]] inline std::vector<PaletteCommand> NeuralPaletteCommands()
    {
        using Common::Settings::NeuralKernelPath;
        const std::pair<const char*, std::optional<NeuralKernelPath>> choices[] = {
             { "Auto", std::nullopt },
             { "Fp32", NeuralKernelPath::Fp32 },
             { "Fp16", NeuralKernelPath::Fp16 },
             { "CoopMatFp16", NeuralKernelPath::CoopMatFp16 },
        };

        std::vector<PaletteCommand> commands;
        for ( const auto& [name, forced] : choices )
            commands.push_back(
                 { "Neural", std::format( "Neural: Kernel path {}", name ),
                   [forced, name]() -> Common::BoolResultStr
                   {
                       const auto resolved = Engine::Neural::ResolveNeuralKernelPath(
                            EngineContext::GetInstance().GetCapabilities().Neural, forced );
                       if ( !resolved )
                           LOG_WARN( "[Neural] forced kernel path {} is set; this device cannot run it: {}", name,
                                     resolved.GetError() );
                       Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath = forced;
                       if ( !Common::Settings::MachineSettings::Save() )
                           return Common::MakeError( "the NN kernel path is set but machine.json was not saved" );
                       return PaletteCommandDone();
                   } } );
        return commands;
    }
} // namespace Desert::Editor

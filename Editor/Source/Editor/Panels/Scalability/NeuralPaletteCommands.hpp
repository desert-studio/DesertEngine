#pragma once

#include <Editor/Core/CommandPalette.hpp>

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
    // "... Fp32" / "Fp16" / "CoopMatFp16" force that path. A path the device cannot run is refused here, with what
    // it lacks (Neural::ResolveNeuralKernelPath), and machine.json is left as it was. The control channel reaches
    // them as `run Neural "Neural: Kernel path Fp16"`; DesertCtl nn-check then runs the forced path.
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
                 { "Neural", std::format( "Neural: Kernel path {}", name ), [forced]() -> Common::BoolResultStr
                   {
                       const auto resolved = Engine::Neural::ResolveNeuralKernelPath(
                            EngineContext::GetInstance().GetCapabilities().Neural, forced );
                       if ( !resolved )
                           return Common::MakeError( resolved.GetError() );
                       Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath = forced;
                       if ( !Common::Settings::MachineSettings::Save() )
                           return Common::MakeError( "the NN kernel path is set but machine.json was not saved" );
                       return PaletteCommandDone();
                   } } );
        return commands;
    }
} // namespace Desert::Editor

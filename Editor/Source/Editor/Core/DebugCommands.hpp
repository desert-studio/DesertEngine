#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // The Debug group of the command palette. Two functions because the group sits in two places of the
    // palette's order: the deliberate crash near the top, the allocator census among the scene-view entries.
    void AppendCrashCommand( std::vector<PaletteCommand>& commands );
    void AppendGpuAllocationCommand( std::vector<PaletteCommand>& commands );
    // "Profiler.MeshDrawSplit 1 / 0" (FPS-MEASURE): Profiler::MeshDrawSplit, the mesh passes' timestamp pair per
    // draw group (UE r.EmitMeshDrawEvents) - the project's profiler switches are palette commands, not cvars.
    void AppendProfilerDrawSplitCommands( std::vector<PaletteCommand>& commands );
    // "Debug.Particle Debugger" (VFX-16): opens the Particle Debugger panel, so an MCP run_command can show it.
    // "Debug.Particles: Freeze / Unfreeze / Step" are the panel's Freeze checkbox and Step button on the active
    // scene's effects world (UE Niagara Debugger's playback controls), so a scenario can stop the effects at a
    // chosen fixed step and photograph that moment instead of whatever frame the clock lands on.
    void AppendParticleDebuggerCommand( std::vector<PaletteCommand>&                  commands,
                                        const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

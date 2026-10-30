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
    // UE `ProfileGPU` / `stat gpu` for a scenario: "Debug.GPU profiling: On / Off" switch the timestamp scopes
    // (Profiler::GpuEnabled, default off - it taxes the frame), "Debug.Log GPU pass timings" writes one log line
    // per GPU pass of the last averaging window (self and inclusive ms), so a pass's cost is read from its own
    // row, never from a difference between two frames.
    void AppendGpuProfileCommands( std::vector<PaletteCommand>& commands );
    // "Debug.Particle Debugger" (VFX-16): opens the Particle Debugger panel, so an MCP run_command can show it.
    // "Debug.Particles: Freeze / Unfreeze / Step" are the panel's Freeze checkbox and Step button on the active
    // scene's effects world (UE Niagara Debugger's playback controls), so a scenario can stop the effects at a
    // chosen fixed step and photograph that moment instead of whatever frame the clock lands on.
    void AppendParticleDebuggerCommand( std::vector<PaletteCommand>&                  commands,
                                        const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor

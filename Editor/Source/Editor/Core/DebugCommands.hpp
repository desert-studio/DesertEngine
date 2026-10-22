#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <vector>

namespace Desert::Editor
{
    // The Debug group of the command palette. Two functions because the group sits in two places of the
    // palette's order: the deliberate crash near the top, the allocator census among the scene-view entries.
    void AppendCrashCommand( std::vector<PaletteCommand>& commands );
    void AppendGpuAllocationCommand( std::vector<PaletteCommand>& commands );
    // "Debug.Particle Debugger" (VFX-16): opens the Particle Debugger panel, so an MCP run_command can show it.
    void AppendParticleDebuggerCommand( std::vector<PaletteCommand>& commands );
} // namespace Desert::Editor

#pragma once

#include <ImGui/imgui.h>

namespace Desert::VFX
{
    struct VFXDebugStats;
}

namespace Desert::Editor
{
    /// VFX-16 (UE Niagara Debugger HUD, FNiagaraDebugHud::DrawOverview): the effects world's systems and emitters
    /// with their alive / capacity counts, channel overflow and the frozen state, drawn top-right under the axis
    /// triad while VFXDebugSettings::Overview is on. The rows are VFXWorld::GetDebugStats, as the panel's.
    void DrawVFXOverviewOverlay( const VFX::VFXDebugStats& stats, const ImVec2& viewportMin,
                                 const ImVec2& viewportMax );
} // namespace Desert::Editor

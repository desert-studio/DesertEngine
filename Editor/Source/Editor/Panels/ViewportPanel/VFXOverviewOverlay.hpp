#pragma once

#include <ImGui/imgui.h>

#include <span>

namespace Desert::Graphic::System
{
    struct ParticleViewEmitterCull;
}

namespace Desert::VFX
{
    struct VFXDebugStats;
}

namespace Desert::Editor
{
    /// VFX-16 (UE Niagara Debugger HUD, FNiagaraDebugHud::DrawOverview): the effects world's systems and emitters
    /// with their alive / capacity counts, channel overflow and the frozen state, drawn top-right under the axis
    /// triad while VFXDebugSettings::Overview is on. The rows are VFXWorld::GetDebugStats, as the panel's; an
    /// emitter this view culled by its fixed Bounds (@p culls, SceneRenderer::ParticleViewEmitterCulls) is marked
    /// "(culled)" and counted in the header row (UE stat Niagara's culled count).
    void DrawVFXOverviewOverlay( const VFX::VFXDebugStats&                                  stats,
                                 std::span<const Graphic::System::ParticleViewEmitterCull> culls,
                                 const ImVec2& viewportMin, const ImVec2& viewportMax );
} // namespace Desert::Editor

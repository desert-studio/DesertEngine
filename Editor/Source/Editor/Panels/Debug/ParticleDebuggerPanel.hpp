#pragma once

#include "Editor/Panels/IPanel.hpp"

#include <Engine/Graphic/Systems/Scene/Particles/ParticleWorldGpu.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    /**
     * @brief "Particle Debugger" - UE's Niagara Debugger for the scene's GPU particles (VFX-16, dev only).
     *
     * While the panel is open it asks the scene's ParticleWorldGpu for the debug readback
     * (ParticleWorldGpu::SetDebugRequest): every emitter's live-particle and free counts as the GPU's last compact
     * left them (read back FramesInFlight frames later, no stall, no CPU-side count), and for the emitter selected
     * in the list one page of its alive particles' attributes (pool index + every float component of the compiled
     * layout, then every int component). Closing the panel withdraws the request, so the node and its copies cost
     * nothing outside debugging. As UE's debugger HUD (FNiagaraDebugHUDSettingsData::ParticlesVariables) the table
     * shows only the attributes whose name contains the filter text. The emitter rows are VFXWorld::GetDebugStats
     * (the same source as the viewport overview); Freeze pauses the world's simulation (VFXDebugSettings::Frozen,
     * every view holds the frame, its pages stay browsable) and Step runs one fixed step; the panel also switches
     * the viewport overview and the bounds draw.
     */
    class ParticleDebuggerPanel final : public IPanel
    {
    public:
        explicit ParticleDebuggerPanel( const std::shared_ptr<Core::Scene>& scene );

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Core::Scene>& scene ) override;

    private:
        // Sends the request (the panel's visibility, the selection, the page) to the scene's particles.
        void SendRequest( bool enabled ) const;

        std::weak_ptr<Core::Scene> m_Scene;
        uint32_t                   m_EntityId = 0;
        uint32_t                   m_Emitter  = 0;
        uint32_t                   m_FirstRow = 0;
        bool                       m_Selected = false;
        char                       m_Filter[64]{};
    };
} // namespace Desert::Editor

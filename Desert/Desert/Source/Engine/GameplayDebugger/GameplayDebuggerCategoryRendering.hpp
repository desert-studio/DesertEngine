#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

#include <string_view>

namespace Desert::Debug
{
    /// What the renderer did with the target (UE has no stock rendering category; this is the per-primitive half
    /// of its "stat" / ShowFlag readouts, in the gameplay debugger's form): every draw record the main view queued
    /// for it - which queue, which LOD of how many (auto, forced or biased), the draw path and the pass that
    /// shades it, the material instance, its template, blend and shading model, whether it casts and by which sun
    /// shadow method (CSM / VSM / ray traced), whether it is in the camera's frustum, its GpuScene primitive - and
    /// draws each record's world bounds.
    ///
    /// The category does not look at the renderer's queues: they are empty while it collects (filled in the render
    /// phase, cleared by SceneRenderer::EndScene). It asks the main view to watch the target
    /// (SceneRenderer::WatchPrimitive) and shows the answer of the last EndScene, one frame late; until that
    /// answer names the target it says it is waiting.
    class GameplayDebuggerCategoryRendering final : public IGameplayDebuggerCategory
    {
    public:
        [[nodiscard]] std::string_view GetName() const override
        {
            return "Rendering";
        }

        void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                          GameplayDebuggerCategoryData& out ) override;
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS

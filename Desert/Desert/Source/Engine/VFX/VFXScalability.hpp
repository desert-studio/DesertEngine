#pragma once

// VFX-11 p7. THE SCALABILITY DECISION as a pure function (UE FNiagaraScalabilityManager::Update with the effect
// type's significance handler): given this tick's views and every system instance of the world, which are culled
// and why, how significant each is and the spawn scale it plays at. VFXWorld::Tick gathers the candidates and
// applies the verdicts (the cull reaction, the log); nothing here touches the registry, an asset or the clock.

#include <Engine/Assets/Serialization/VFXEffectType.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::VFX
{
    /// One view's report for the next decision (SceneRenderer, once per frame per main view).
    struct VFXViewReport
    {
        glm::vec3 Origin{ 0.0f };        // centimetres
        double    SpawnCountScale = 1.0; // Effects.SpawnCountScalePercent / 100
        int       EffectsLevel    = 0;   // Effects.QualityLevel: the level of every effect type this view reads
        std::uint32_t MaxWorldParticles = 0; // Effects.MaxWorldParticles: the world pool budget; 0 = none
        // The view's frustum as six inward planes (xyz normal, w distance; a point p is inside when
        // dot(n, p) + w >= 0 for all six). HasFrustum false = the view culls nothing by frustum.
        std::array<glm::vec4, 6> FrustumPlanes{};
        bool                     HasFrustum = false;
    };

    enum class VFXCullReason : std::uint8_t
    {
        None,
        Distance,           // past MaxDistance at every view's level
        ViewFrustum,        // CullByViewFrustum and outside every view's frustum
        NotRendered,        // outside every view's frustum for longer than MaxTimeWithoutRender
        SystemInstanceCount, // over MaxSystemInstances of its system, and less significant than those kept
        InstanceCount,      // over MaxInstances of its type, and less significant than those kept
        WorldBudget,        // its pool slots would take the world past Effects.MaxWorldParticles; less significant
                            // than every system the budget holds
    };

    struct VFXScalabilityCandidate
    {
        const Assets::Serialization::VFXEffectTypeData* Type   = nullptr; // nullptr = a missing type: culled
        const void*                                     System = nullptr; // the system asset (MaxSystemInstances key)
        glm::vec3 BoundsMinCm{ 0.0f }; // world-space bounds, centimetres
        glm::vec3 BoundsMaxCm{ 0.0f };
        double    AgeSeconds    = 0.0; // since the instance started (Age significance)
        double    HiddenSeconds = 0.0; // outside every frustum before this tick
        std::uint32_t PoolParticles = 0; // the pool slots it holds while playing (its running emitters' Capacity)
    };

    struct VFXScalabilityVerdict
    {
        VFXCullReason Reason          = VFXCullReason::None;
        float         NearestCm       = 0.0f; // to the nearest view; infinity without views
        double        Significance    = 0.0;  // higher = more significant
        double        SpawnCountScale = 1.0;
        double        HiddenSeconds   = 0.0; // the candidate's HiddenSeconds after this tick (0 when visible)
        // The world budget this tick decided by (0 = none) and the pool slots the more significant systems kept
        // took before this one - the log's "why" for a WorldBudget refusal.
        std::uint64_t BudgetParticles = 0;
        std::uint64_t BudgetTakenBefore = 0;
    };

    /// The verdict of every candidate, in their order. No view (headless) = nothing culled, scale 1.
    std::vector<VFXScalabilityVerdict> EvaluateVFXScalability( std::span<const VFXViewReport>          views,
                                                               std::span<const VFXScalabilityCandidate> candidates,
                                                               double                                   tickSeconds );

    /// "distance", "view frustum", ... for the log.
    const char* VFXCullReasonName( VFXCullReason reason );
} // namespace Desert::VFX

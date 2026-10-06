#pragma once

// VFX-06. HOW MANY PARTICLES AN EMITTER BEARS IN ONE FIXED STEP — the CPU half of the emitter stack (plan 02
// §3.3-3: spawn budgets, bursts and the lifecycle are administration, decided once per world, not simulation).
//
// The emitter's EmitterUpdate group is not compiled to GPU code (VFXStackCompiler): its modules are read here into
// a SPAWN PLAN, and the world steps a per-instance SpawnState through it once per fixed step. Port of UE's
// stateless spawn infos (NiagaraStatelessSpawnInfo.h:13-76 — Burst: SpawnTime + Amount; Rate: Rate) and of the
// loop windowing in FNiagaraStatelessEmitterInstance::InitSpawnInfosForLoop
// (NiagaraStatelessEmitterInstance.cpp:695-890): a burst fires once in every loop whose window holds its time, a
// rate spawns over the part of the step that lies inside an active loop. UE computes a whole loop's count up front
// (Amount = floor(LoopDuration * Rate), :887); our simulation is stateful, so the rate goes through the per-step
// SpawnAccumulator instead (the fraction carries to the next step) — over N steps the same floor(N * dt * Rate).
//
// Engine CPU modules (EmitterUpdate group):
//   engine:SpawnRate   Input SpawnRate float  — particles per second while a loop is active
//   engine:SpawnBurst  Input SpawnCount int, Input SpawnTime float — SpawnCount particles once per loop, at
//                      SpawnTime seconds into it (0 <= SpawnTime < LoopDuration)
// An input is a Value or a User.* binding (read from the system's UserParams). A curve over emitter time, a
// random range or a Particles.* binding have no meaning on the CPU side yet and are refused by name.

#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXClock.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::VFX
{
    inline constexpr std::string_view kVFXSpawnRateModule  = "engine:SpawnRate";
    inline constexpr std::string_view kVFXSpawnBurstModule = "engine:SpawnBurst";

    /// UE FNiagaraStatelessSpawnInfo, Type Burst.
    struct VFXSpawnBurst
    {
        float         Time  = 0.0f; ///< seconds into each loop
        std::uint32_t Count = 0;

        [[nodiscard]] bool operator==( const VFXSpawnBurst& ) const = default;
    };

    /// Everything the CPU needs to count one emitter's births.
    struct VFXSpawnPlan
    {
        Assets::Serialization::VFXEmitterLifecycle Lifecycle;
        double                                     Rate = 0.0; ///< the sum of every SpawnRate module, per second
        std::vector<VFXSpawnBurst>                 Bursts;

        [[nodiscard]] bool operator==( const VFXSpawnPlan& ) const = default;
    };

    /// Reads emitter @p emitterIndex's lifecycle and EmitterUpdate group. A module other than the two above, a
    /// local module in the group, an input the module does not take or leaves out, a source it cannot read and a
    /// burst outside its loop are errors naming the emitter, module and input.
    Common::ResultStr<VFXSpawnPlan> CompileSpawnPlan( const Assets::Serialization::VFXSystemData& system,
                                                      std::size_t                                 emitterIndex );

    /// One emitter instance's spawn state: the time it has been running and the rate's carried fraction. Reset
    /// by assigning a fresh value.
    struct SpawnState
    {
        double           Age = 0.0; ///< seconds since the instance (re)started, including the lifecycle Delay
        SpawnAccumulator RateCarry;

        /// The births of the next fixed step of @p stepSeconds: the step covers [Age, Age + stepSeconds).
        [[nodiscard]] std::uint32_t Step( const VFXSpawnPlan& plan, double stepSeconds );
    };
} // namespace Desert::VFX

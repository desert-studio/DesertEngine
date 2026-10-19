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
// Engine CPU modules (EmitterUpdate group), each a `.dfxmod` of the engine module library that a row names by
// GUID; a row whose module has no behaviour here is refused naming its GUID and path:
//   SpawnRate          Input SpawnRate float  — particles per second while a loop is active
//   SpawnBurst         Input SpawnCount int, Input SpawnTime float — SpawnCount particles once per loop, at
//                      SpawnTime seconds into it (0 <= SpawnTime < LoopDuration)
//   SpawnFromChannel   (VFX-10, UE "Spawn From Data Channel"; VFX/VFXDataChannel.hpp)
//                      Channel           Binding DataChannel.<channel>
//                      ParticlesPerEntry Int >= 1, MaxEntriesPerFrame Int >= 1, MaxDistance Float cm >= 0
//                      (optional) Position / Direction Vec3 Binding DataChannel.<channel>.<field> (optional)
//                      Filter Float Binding DataChannel.<channel>.<field> with FilterOp Int (VFXChannelFilterOp)
//                      and FilterValue Float (all three or none)
// An input is a Value or a User.* binding (read from the system's UserParams). A curve over emitter time, a
// random range or a Particles.* binding have no meaning on the CPU side yet and are refused by name.

#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXClock.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert::VFX
{
    /// An engine module whose EmitterUpdate stage is C++ below: the GUID its `.dfxmod` header states (the file
    /// in the engine module library, Editor/Resources/Engine/VFX/Modules) and that file's stable key. A stack
    /// row reaches the behaviour through the GUID alone; the key is what a row the engine writes states as Path.
    struct VFXEngineCpuModule
    {
        Common::Content::AssetGuid Guid;
        std::string_view           Name;
        std::string_view           Path;
    };

    inline constexpr VFXEngineCpuModule kVFXSpawnRateModule{ { 0x10e483e233d54710ull, 0x8e31d1855328a54bull },
                                                             "SpawnRate",
                                                             "engine:Engine/VFX/Modules/SpawnRate.dfxmod" };
    inline constexpr VFXEngineCpuModule kVFXSpawnBurstModule{ { 0x02637a2ef2774901ull, 0xa0a88452af542abdull },
                                                              "SpawnBurst",
                                                              "engine:Engine/VFX/Modules/SpawnBurst.dfxmod" };
    inline constexpr VFXEngineCpuModule kVFXSpawnFromChannelModule{
         { 0xc3e1a7d94b2f4e6aull, 0x8d05f1b97a6e2c48ull },
         "SpawnFromChannel",
         "engine:Engine/VFX/Modules/SpawnFromChannel.dfxmod" };
    // VFX-PARAMS: the particle half of an Event Handler. The stack compiler appends it as the last spawn module of
    // every emitter that states EventHandlers; a stack never names it.
    inline constexpr VFXEngineCpuModule kVFXSpawnFromEventModule{
         { 0x0f58cf1ce255404dull, 0x8a2e694ae1f8a429ull },
         "SpawnFromEvent",
         "engine:Engine/VFX/Modules/SpawnFromEvent.dfxmod" };

    /// The stack row reference to @p module ({Guid, Path}).
    [[nodiscard]] Assets::AssetGuidRef ModuleRef( const VFXEngineCpuModule& module );

    /// Whether the engine has C++ for the EmitterUpdate stage of the module of @p guid (one of the three above).
    [[nodiscard]] bool HasCpuBehaviour( const Common::Content::AssetGuid& guid );

    /// Whether @p use names @p module (by GUID; the Path is not compared).
    [[nodiscard]] bool UsesModule( const Assets::Serialization::VFXModuleUse& use,
                                   const VFXEngineCpuModule&                  module );

    /// UE FNiagaraStatelessSpawnInfo, Type Burst.
    struct VFXSpawnBurst
    {
        float         Time  = 0.0f; ///< seconds into each loop (the constant; TimeUser replaces it when bound)
        std::uint32_t Count = 0;    ///< the constant; CountUser replaces it when bound
        /// User rows (VFXSystemData::UserParams index) read per instance at each step: a resolved time outside
        /// [0, LoopDuration) never fires, a resolved count is max(0, round(v)).
        std::optional<std::uint32_t> TimeUser;
        std::optional<std::uint32_t> CountUser;

        [[nodiscard]] bool operator==( const VFXSpawnBurst& ) const = default;
    };

    /// Everything the CPU needs to count one emitter's births.
    struct VFXSpawnPlan
    {
        Assets::Serialization::VFXEmitterLifecycle Lifecycle;
        double Rate = 0.0; ///< the sum of every SpawnRate module whose rate is a constant, per second
        /// User rows of the User-bound SpawnRate modules (UE reads a User-bound SpawnRate from the component's
        /// override parameters, so the row is kept, not its default baked in), summed per step.
        std::vector<std::uint32_t> UserRates;
        std::vector<glm::vec4>     UserDefaults; ///< the system's User defaults, for a step given no values

        /// The total rate per second for these resolved User values (UserDefaults when empty); a User-bound
        /// rate below zero counts as zero (UE SpawnRate clamps).
        [[nodiscard]] double                       RateFor( std::span<const glm::vec4> user ) const;
        std::vector<VFXSpawnBurst>                 Bursts;
        /// engine:SpawnFromChannel, at most one per emitter: its spawns join the first fixed step of each tick.
        std::optional<VFXChannelSpawnModule> Channel;

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
        /// Births in the next @p stepSeconds. @p user = the instance's resolved User values this tick
        /// (VFXUserParams.hpp ResolveUserParams), one per UserParams row; empty reads the system defaults.
        [[nodiscard]] std::uint32_t Step( const VFXSpawnPlan& plan, double stepSeconds,
                                          std::span<const glm::vec4> user = {} );
    };
} // namespace Desert::VFX

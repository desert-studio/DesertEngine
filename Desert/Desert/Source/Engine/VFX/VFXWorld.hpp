#pragma once

#include <Engine/VFX/VFXClock.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::VFX
{
    // One fixed step of one emitter, as the simulation will run it.
    struct EmitterStep
    {
        // Id of the first particle this step may spawn; the step spawns ids [IdBase, IdBase + Budget).
        // Ids are reserved whether or not a free slot takes them, so the id — and every random number
        // drawn from it — never depends on how many particles happened to be alive.
        std::uint32_t IdBase = 0;
        std::uint32_t Budget = 0;
        // VFX-10: the first ChannelCount of the Budget's spawns are Spawn from Channel particles; spawn t <
        // ChannelCount takes particle ChannelFirst + t of the instance's ChannelSpawns (expanded per particle).
        std::uint32_t ChannelFirst = 0;
        std::uint32_t ChannelCount = 0;
    };

    // The world's administration of one emitter instance: everything about it that is decided on the
    // CPU, once per world, and that every view of the world must agree on.
    struct EmitterInstance
    {
        std::uint32_t Seed = 0;

        // Changes every time this instance's simulated state is thrown away (a reset, a backwards seek,
        // the editor's Restart). A renderer holding GPU state of another generation zeroes it before the
        // steps below. Unique across the whole process, so state can never be mistaken for a later one.
        std::uint64_t Generation = 0;

        std::uint32_t NextId = 0; // particle ids handed out since the last reset
        SpawnState    Spawn;      // the births of each step (VFXEmitterSpawn): lifecycle, rate, bursts

        // This tick's steps, in order; empty when the clock ran none or the emitter is disabled.
        std::vector<EmitterStep> Steps;

        // VFX-10: this tick's Spawn from Channel requests (empty without the module) and what became of the
        // channel's entries; ChannelOverflowTotal counts every entry ever refused by the per-frame limit.
        std::vector<VFXChannelSpawnRequest> ChannelSpawns;
        VFXChannelSpawnReport               ChannelReport;
        std::uint64_t                       ChannelOverflowTotal = 0;

        bool Seen = false; // visited by the current Tick; instances not visited are dropped
    };

    /// One instance's steps for a tick of @p stepCount fixed steps: the plan's rate and bursts per step, and -
    /// when the plan holds engine:SpawnFromChannel - this frame's entries of its channel in @p channels gathered
    /// for an emitter at @p emitterCm, all joining the first step (none runs: they count as Overflow). Ids are
    /// reserved for every spawn, channel spawns included.
    void PlanEmitterSteps( EmitterInstance& instance, const VFXSpawnPlan& plan, std::uint32_t stepCount,
                           double stepSeconds, const VFXDataChannels& channels, const glm::vec3& emitterCm );

    // THE EFFECTS WORLD OF ONE SCENE (plan 02 §3.3-1): owns the effects clock and the per-instance
    // administration, and is ticked once per scene update — not once per view. Simulation is a function
    // of the world: two viewports of one scene read the same plan, so they cannot run two different
    // simulations (§1.3-8), and a frame is a function of the scene's time, not of the wall clock.
    //
    // The render side of a world's effects (UE: FScene::FXSystem - the scene's GPU particle state), made by the
    // renderer on first use (Graphic ParticleWorldGpu) and owned by the world, so every view of the scene shares
    // it. Engine/VFX does not know what is in it.
    class WorldGpuState
    {
    public:
        virtual ~WorldGpuState() = default;
    };

    // Today the instances are ParticleEmitterComponents, keyed by entity UUID; their GPU state is the world's
    // WorldGpuState (VFX-07b), one per scene, not one per view.
    class VFXWorld
    {
    public:
        explicit VFXWorld( const ClockSettings& settings = {} ) : m_Clock( settings )
        {
        }

        // One scene update. `seconds` is the scene's own time for this update: the editor delta in Edit,
        // the gameplay delta in Play, zero while paused. Consumes ParticleEmitterComponent::RequestRestart and
        // the data channels' entries written since the last tick.
        void Tick( entt::registry& registry, double seconds );

        // DesiredAge: the next ticks replay to `age` (a backwards seek resets every instance first).
        void SeekTo( double age )
        {
            m_Clock.SeekTo( age );
        }
        void Reset()
        {
            m_Clock.Reset();
        }

        // A world emptied with its scene: the clock back to zero, every instance forgotten.
        void Clear();

        [[nodiscard]] const Clock& GetClock() const
        {
            return m_Clock;
        }
        [[nodiscard]] const TickPlan& GetPlan() const
        {
            return m_Plan;
        }
        [[nodiscard]] const EmitterInstance* FindEmitter( std::uint64_t entityUuid ) const;

        // The scene's data channels (VFX-10): gameplay registers channels and writes entries here (C++ Write,
        // Lua VFX.writeChannel); the next Tick spawns from them and clears them.
        [[nodiscard]] VFXDataChannels& GetDataChannels()
        {
            return m_Channels;
        }
        [[nodiscard]] const VFXDataChannels& GetDataChannels() const
        {
            return m_Channels;
        }

        // Counts Tick calls: a renderer runs one tick's simulation once, in the first view that sees its serial.
        [[nodiscard]] std::uint64_t GetTickSerial() const
        {
            return m_TickSerial;
        }

        // The world's render-side state (see WorldGpuState). Const: the renderer is handed a const scene, and the
        // GPU state is not part of the world's simulated state (nothing CPU-side reads it); Clear drops it.
        [[nodiscard]] WorldGpuState* GetGpuState() const
        {
            return m_GpuState.get();
        }
        void SetGpuState( std::unique_ptr<WorldGpuState> state ) const
        {
            m_GpuState = std::move( state );
        }

    private:
        void ResetInstance( EmitterInstance& instance );

        Clock                                              m_Clock;
        TickPlan                                           m_Plan;
        std::unordered_map<std::uint64_t, EmitterInstance> m_Emitters;
        std::uint64_t                                      m_LastGeneration = 0;
        std::uint64_t                                      m_TickSerial     = 0;
        VFXDataChannels                                    m_Channels;
        mutable std::unique_ptr<WorldGpuState>             m_GpuState;
    };
} // namespace Desert::VFX

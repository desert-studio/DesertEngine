#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace Desert::Debug
{
    /// The target's physics (UE has no stock physics category; this is the Chaos debugger's per-body readout
    /// in the gameplay debugger's form): what moves it - a rigid body (its type, mass, profile) or a character
    /// controller (ground, crouch, swim) -, its velocity as the physics world holds it, whether Jolt keeps the
    /// body awake, and its contacts - the step's Hit / Overlap facts that name it (PhysicsEventQueue), plus the
    /// last few seen while the debugger watched this target. Draws the velocity (one second of travel) and
    /// every contact of the step as its point and normal.
    ///
    /// The recent-contact list is the category's own memory of the queues it read (the queue holds one frame);
    /// it starts empty for a new target and is not filled while the debugger is hidden, which its title says.
    class GameplayDebuggerCategoryPhysics final : public IGameplayDebuggerCategory
    {
    public:
        [[nodiscard]] std::string_view GetName() const override
        {
            return "Physics";
        }

        void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                          GameplayDebuggerCategoryData& out ) override;

    private:
        struct SeenContact
        {
            std::string Text; // "Hit Crate 120.0 kg*cm/s", as the panel shows it
        };

        void ReadQueue( const Core::Scene& scene, entt::entity target );

        std::optional<entt::entity> m_Watched;             // the target m_Recent was gathered for
        std::uint64_t               m_ReadPublication = 0; // PhysicsEventQueue::Publication last read
        std::deque<SeenContact>     m_Recent;              // newest first
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS

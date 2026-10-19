#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

namespace Desert::Debug
{
    /// The engine's own category: what any entity is - its name, UUID, world transform, velocity when it moves
    /// by a character controller, and the components it carries (the scene format's component table, so every
    /// component a scene can hold is named by its file key). Marks the target with a cross and its name; a
    /// moving one with its velocity as an arrow (one second of travel).
    class GameplayDebuggerCategoryBasic final : public IGameplayDebuggerCategory
    {
    public:
        [[nodiscard]] std::string_view GetName() const override
        {
            return "Basic";
        }

        void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                          GameplayDebuggerCategoryData& out ) override;
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS

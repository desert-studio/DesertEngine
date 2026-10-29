#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

namespace Desert::Debug
{
    /// The target's scripts (UE: a Blueprint's debug view of an actor - its class, its variables' live values,
    /// the last script error): every slot of its ScriptComponent - the script file, whether it started, why it is
    /// not running as written (ScriptSlot::RuntimeError: its load error, else its last OnUpdate error) - and
    /// every property the slot's script declares with the value the running script holds now
    /// (ScriptEngine::ReadLiveProperties), marked when the slot overrides the script's default.
    class GameplayDebuggerCategoryScript final : public IGameplayDebuggerCategory
    {
    public:
        [[nodiscard]] std::string_view GetName() const override
        {
            return "Script";
        }

        void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                          GameplayDebuggerCategoryData& out ) override;
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS

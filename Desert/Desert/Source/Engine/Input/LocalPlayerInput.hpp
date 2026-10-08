#pragma once

#include <Engine/Input/EnhancedInputSubsystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>
#include <glm/vec2.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::ECS
{
    struct EnhancedInputPlayerData;
}

// THE LOCAL PLAYER'S ENHANCED INPUT (GP1b) — UE's ULocalPlayer + its UEnhancedInputLocalPlayerSubsystem: the one
// EnhancedInputSubsystem of the one local player, owned by the script runtime of the world being played
// (ScriptSystem ticks it once per frame BEFORE the scripts run, so OnUpdate reads this frame's events), with the
// two things the bare subsystem has no business knowing: where its contexts come from (assets, by handle from
// EnhancedInputPlayerComponent or by name from Lua) and the names scripts call actions and contexts by (their
// file names, IA_Jump / IMC_Default — the Lua `Input` table names keys the same way, "W", "Space").
namespace Desert::Input
{
    /// One frame of device state for @p keys only (the keys the active mappings read), through the given
    /// device readers — so the sampling is the same code in a test and in the game.
    RawInputFrame SampleRawInput( const std::vector<InputKey>& keys, glm::vec2 mouseDelta,
                                  const std::function<bool( Common::KeyCode )>&     keyDown,
                                  const std::function<bool( Common::MouseButton )>& buttonDown );

    /// The priority each of @p player's contexts is added at: entry i gets BasePriority + (count - 1 - i).
    std::vector<std::pair<Assets::AssetHandle, int>>
    PlayerContextPriorities( const ECS::EnhancedInputPlayerData& player );

    class LocalPlayerInput
    {
    public:
        /// UE BeginPlay: a fresh subsystem, this user's saved key overrides, then every context every
        /// EnhancedInputPlayerComponent of @p registry names. A context that cannot be added is logged by name
        /// and the others still are.
        void BeginPlay( entt::registry& registry, Assets::AssetManager& assets );
        /// UE EndPlay: every context removed and every action forgotten.
        void EndPlay();

        /// Samples the devices for the mapped keys and evaluates one frame.
        void Tick( glm::vec2 mouseDelta, float deltaSeconds );

        /// The context named @p name (its file name without extension, or its content path) read through the
        /// content registry and added at @p priority, with every action it maps.
        Common::BoolResultStr AddContext( Assets::AssetManager& assets, const std::string& name, int priority );
        /// false when no active context has that name.
        bool RemoveContext( const std::string& name );

        /// A context whose data and actions are already read (what AddContext does once the files are in):
        /// registers each action under its GUID and its file name, then adds the context under @p name.
        Common::BoolResultStr
        AddLoadedContext( const std::string& name, const Assets::Serialization::InputMappingContextData& context,
                          const std::map<std::string, Assets::Serialization::InputActionData>& actionsByGuidText,
                          int                                                                  priority );

        /// The action a script calls @p name (IA_Jump), when a context that maps it was added.
        [[nodiscard]] std::optional<Common::Content::AssetGuid> ActionNamed( const std::string& name ) const;
        [[nodiscard]] std::optional<Common::Content::AssetGuid> ContextNamed( const std::string& name ) const;

        /// The player rebinds (UE MapPlayerKey): @p key in place of @p defaultKey for @p action in @p context,
        /// effective at once and written to this user's input.json (never to the asset).
        Common::BoolResultStr RebindKey( const std::string& context, const std::string& action,
                                         const std::string& defaultKey, const std::string& key );

        [[nodiscard]] EnhancedInputSubsystem& Subsystem()
        {
            return m_Subsystem;
        }
        [[nodiscard]] const EnhancedInputSubsystem& Subsystem() const
        {
            return m_Subsystem;
        }

    private:
        EnhancedInputSubsystem                            m_Subsystem;
        std::map<std::string, Common::Content::AssetGuid> m_ActionNames;
        std::map<std::string, Common::Content::AssetGuid> m_ContextNames;
    };
} // namespace Desert::Input

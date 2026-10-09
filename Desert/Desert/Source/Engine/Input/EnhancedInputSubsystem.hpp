#pragma once

#include <Engine/Assets/Serialization/InputAssets.hpp>
#include <Engine/Input/InputKey.hpp>
#include <Engine/Input/UserKeyBindings.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// THE ENHANCED INPUT SUBSYSTEM (GP1a) — UE's UEnhancedInputLocalPlayerSubsystem + UEnhancedPlayerInput, as one
// plain object a player owns. It holds the registered actions and a stack of active mapping contexts with
// priorities; Tick(frame, dt) turns one RawInputFrame into every action's value and its trigger events, the
// same way every time for the same frames (no device is read here, no clock but dt).
//
// PER FRAME, as UE evaluates it:
//   1. The active mappings, highest priority first (equal priorities: the earlier-added context first). A key
//      a higher-priority context maps to a ConsumeInput action is not seen by any lower context.
//   2. Each mapping: the key's raw value, shaped to the action's value type, then its modifiers IN ORDER,
//      shaped again; then its triggers on that value (no triggers: triggered while non-zero).
//   3. Each action: the strongest state of its mappings (Triggered > Ongoing > None) and the per-component
//      highest-absolute value of its actuated mappings (UE TakeHighestAbsoluteValue); value zero when None.
//   4. The event: the transition from last frame's state to this one (UE ETriggerEventInternal).
namespace Desert::Input
{
    /// UE's ETriggerState.
    enum class TriggerState : uint8_t
    {
        None,
        Ongoing,
        Triggered,
    };

    /// UE's ETriggerEvent as flags: None->Triggered fires Started AND Triggered in the same frame (UE
    /// StartedAndTriggered).
    struct TriggerEvents
    {
        bool Started   = false;
        bool Ongoing   = false;
        bool Triggered = false;
        bool Completed = false;
        bool Canceled  = false;

        [[nodiscard]] bool Any() const
        {
            return Started || Ongoing || Triggered || Completed || Canceled;
        }
        [[nodiscard]] bool operator==( const TriggerEvents& ) const = default;
    };

    /// The events of the transition @p last -> @p now.
    TriggerEvents EventsForTransition( TriggerState last, TriggerState now );

    /// The value after @p modifiers, in order, on a value of @p type (exposed for the modifier tests).
    glm::vec3 ApplyModifiers( glm::vec3 value, Assets::Serialization::InputValueType type,
                              const std::vector<Assets::Serialization::InputModifierData>& modifiers );

    /// @p value with only the components @p type has; a Bool is 1 when any component is non-zero.
    glm::vec3 ShapeToValueType( glm::vec3 value, Assets::Serialization::InputValueType type );

    class EnhancedInputSubsystem
    {
    public:
        /// Makes @p action known under @p guid. Re-registering a GUID replaces its data and rebuilds.
        void RegisterAction( const Common::Content::AssetGuid&             guid,
                             const Assets::Serialization::InputActionData& action );

        /// UE AddMappingContext. The context is identified by its header GUID; every action it maps must be
        /// registered (an unknown action is an error naming it, and the context is not added). Adding a
        /// context already active updates its priority.
        Common::BoolResultStr AddMappingContext( const Assets::Serialization::InputMappingContextData& context,
                                                 int                                                   priority );

        /// UE RemoveMappingContext; false when it was not active. The actions it drove see their mappings
        /// vanish: an action left without input goes to None on the next Tick (Completed / Canceled).
        bool RemoveMappingContext( const Common::Content::AssetGuid& contextGuid );

        /// Evaluates one frame. @p deltaSeconds advances the timed triggers (Hold).
        void Tick( const RawInputFrame& frame, float deltaSeconds );

        /// Every key the active mappings read this frame (after the player's overrides), each once — what the
        /// owner samples from the devices into the RawInputFrame.
        [[nodiscard]] std::vector<InputKey> MappedKeys() const;

        // ---- The player's own keys (UE UEnhancedInputUserSettings::MapPlayerKey) ----

        /// Replaces every override and rebuilds; a mapping an override names reads the override's key.
        void                                 SetUserKeyBindings( UserKeyBindings bindings );
        [[nodiscard]] const UserKeyBindings& GetUserKeyBindings() const
        {
            return m_UserKeys;
        }
        /// The player maps @p key in place of @p defaultKey for @p action in @p context. Mapping a key back to
        /// its default removes the override. An unknown key name is an error naming it.
        Common::BoolResultStr RemapKey( const Common::Content::AssetGuid& context,
                                        const Common::Content::AssetGuid& action, const std::string& defaultKey,
                                        const std::string& key );

        // ---- The C++ query API (UE's FInputActionInstance / BindAction events, polled) ----

        /// The action's value this frame (zero when its state is None or it is unknown).
        [[nodiscard]] glm::vec3     GetActionValue( const Common::Content::AssetGuid& action ) const;
        [[nodiscard]] TriggerState  GetTriggerState( const Common::Content::AssetGuid& action ) const;
        [[nodiscard]] TriggerEvents GetTriggerEvents( const Common::Content::AssetGuid& action ) const;
        /// Seconds the action has been Triggered without a break (UE ElapsedTriggeredTime); 0 when not.
        [[nodiscard]] float GetTriggeredSeconds( const Common::Content::AssetGuid& action ) const;

    private:
        struct TriggerRuntime
        {
            Assets::Serialization::InputTriggerData Data;
            TriggerState                            LastState    = TriggerState::None;
            float                                   HeldDuration = 0.0f;
        };

        struct MappingRuntime
        {
            std::string                           ActionKey;
            Assets::Serialization::InputValueType ValueType = Assets::Serialization::InputValueType::Bool;
            InputKey                              Key;
            std::vector<Assets::Serialization::InputModifierData> Modifiers;
            std::vector<TriggerRuntime>                           Triggers;
            glm::vec3                                             LastValue{ 0.0f };
        };

        struct ActiveContext
        {
            std::string                                    GuidText;
            Assets::Serialization::InputMappingContextData Data;
            int                                            Priority = 0;
            uint64_t                                       Order    = 0;
        };

        struct ActionRuntime
        {
            Assets::Serialization::InputActionData Data;
            TriggerState                           State = TriggerState::None;
            TriggerEvents                          Events;
            glm::vec3                              Value{ 0.0f };
            float                                  TriggeredSeconds = 0.0f;
        };

        void         RebuildMappings();
        static void  CarryMappingState( MappingRuntime& runtime, const std::vector<MappingRuntime>& previous,
                                        std::vector<bool>& carried );
        TriggerState EvaluateTriggers( MappingRuntime& mapping, const glm::vec3& value, float deltaSeconds );

        std::map<std::string, ActionRuntime> m_Actions; // keyed by AssetGuidToText
        std::vector<ActiveContext>           m_Contexts;
        std::vector<MappingRuntime>          m_Mappings; // the evaluation order, rebuilt on any change
        uint64_t                             m_NextOrder = 0;
        UserKeyBindings                      m_UserKeys;
    };
} // namespace Desert::Input

#pragma once

// WHAT THE LEVEL SEQUENCE DRIVES ON A PLACED EFFECT (UE: the Niagara plugin's MovieSceneNiagaraSystemTrack and the
// "Activate" event, which the plugin registers with the Sequencer). The sequencer writes the drive and fires the
// activation through these hooks and never names the VFX module; the module fills them at StartupModule
// (VFX/Private/VFXModule.cpp). Unfilled (an executable without the VFX module): an entity has no effect to drive.

#include <Common/Core/Entt.hpp>

#include <optional>

namespace Desert::ECS
{
    // A VFX track's sample as the effect reads it: whether its section is active, and the effect's age in it.
    struct VFXSequencerDrive
    {
        bool   Active     = false;
        double AgeSeconds = 0.0;

        bool operator==( const VFXSequencerDrive& ) const = default;
    };

    struct SequencerEffectHookTable
    {
        /// The drive slot of @p entity's effect; null when the entity has no effect component.
        std::optional<VFXSequencerDrive>* ( *DriveOf )( entt::registry& registry, entt::entity entity ) = nullptr;
        /// UE UNiagaraComponent::Activate(true): the placed effect starts bearing, from the start. False when the
        /// entity has no effect component.
        bool ( *Activate )( entt::registry& registry, entt::entity entity ) = nullptr;
    };

    inline SequencerEffectHookTable& SequencerEffectHooks()
    {
        static SequencerEffectHookTable hooks;
        return hooks;
    }

    inline std::optional<VFXSequencerDrive>* SequencerEffectDriveOf( entt::registry& registry,
                                                                     entt::entity    entity )
    {
        const auto driveOf = SequencerEffectHooks().DriveOf;
        return driveOf != nullptr ? driveOf( registry, entity ) : nullptr;
    }
} // namespace Desert::ECS

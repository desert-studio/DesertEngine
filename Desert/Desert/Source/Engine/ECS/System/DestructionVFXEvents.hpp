#pragma once

// DESTRUCTION -> VFX (DST-05). UE: Chaos destruction events (FChaosBreakEvent, FChaosCollisionEvent,
// FChaosRemovalEvent) read by the Niagara Chaos data interface. Ours: what the scene's DestructionWorld recorded
// in this frame's physics steps is written, once per frame on the main thread after the step, into the scene's
// VFX data channels, where an emitter's engine:SpawnFromChannel module reads it in the same frame's VFXWorld tick.
//
// THREE CHANNELS, one per event type (as UE keeps three event types): their payloads differ (a collision has a
// normal and an impulse, a break a piece count), each has its own per-frame budget in the reading module (a
// flood of contacts never starves the breaks), and the module's one field predicate stays free for the payload
// (e.g. Impulse >= X) instead of being spent on an EventType field. The layouts are the project's assets
// Content/VFX/DestructionBreak.dfxch, DestructionCollision.dfxch, DestructionRemoved.dfxch - the field names
// below are what this writer requires of them; a layout without one is refused by name.
//
// Filtered per destructible entity (UE bNotifyBreaks / bNotifyCollisions / bNotifyRemovals): DestructibleData's
// NotifyBreaks / NotifyCollisions / NotifyRemovals. Nothing is written for an entity whose flag is off.

#include <Engine/Destruction/DestructionWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::VFX
{
    class VFXDataChannels;
}

namespace Desert::ECS
{
    inline constexpr std::string_view kDestructionBreakChannel     = "DestructionBreak";
    inline constexpr std::string_view kDestructionCollisionChannel = "DestructionCollision";
    inline constexpr std::string_view kDestructionRemovedChannel   = "DestructionRemoved";

    /// The entity a destructible object belongs to and which of its events it publishes.
    struct DestructionEventSource
    {
        int32_t Entity           = -1; ///< entt entity index (fits the channel's exact Int)
        bool    NotifyBreaks     = false;
        bool    NotifyCollisions = false;
        bool    NotifyRemovals   = false;
    };
    using DestructionEventSources = std::unordered_map<Destruction::DestructibleHandle, DestructionEventSource>;

    /// Every simulated destructible entity (DestructibleComponent::RuntimeObject set) by its object handle.
    [[nodiscard]] DestructionEventSources CollectDestructionEventSources( entt::registry& registry );

    /// How many entries of each channel one write made.
    struct DestructionVFXReport
    {
        uint32_t Breaks     = 0;
        uint32_t Collisions = 0;
        uint32_t Removals   = 0;
    };

    /// Registers, through @p assets (VFXDataChannels::Use), each channel some source publishes that @p channels
    /// does not hold yet. Refused by name when a channel asset cannot be found.
    Common::BoolResultStr UseDestructionChannels( const DestructionEventSources& sources,
                                                  VFX::VFXDataChannels& channels, Assets::AssetManager& assets );

    /// Writes every event whose source publishes its kind into its channel; events of an object no source
    /// names are dropped (its entity is gone). Refused by name when a channel with entries to write is not
    /// registered or its layout lacks a field (the entries already appended stay zero for this frame).
    Common::BoolResultStr WriteDestructionEventsToVFX( std::span<const Destruction::DestructionEvent> events,
                                                       const DestructionEventSources&                 sources,
                                                       VFX::VFXDataChannels&                          channels,
                                                       DestructionVFXReport&                          report );
} // namespace Desert::ECS

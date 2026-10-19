#pragma once

// KEEP A PLAY-TIME CHANGE PAST STOP (UE: "Keep Simulation Changes", EditorServer.cpp
// UEditorEngine::CopyActorProperties applied at EndPlayMap).
//
// Stop restores the authored world from the snapshot Play took (PlaySession::Stop), so every edit made
// during Play is discarded — that is the rule, and it stays the rule. A few edits exist only BECAUSE
// Play is running: "Capture current sky" reads the drift game time has given the clouds, which the edit
// world never has. Those ask to be kept here, by the entity's UUID (the counterpart that survives the
// restore, as the selection does), and PlaySession::Stop applies them to the restored world as one
// undoable edit each.
//
// ONE CHANGE PER (entity, key): capturing twice keeps the second capture, not both. Play's start forgets
// whatever an earlier session left unapplied.

#include <Common/Core/UUID.hpp>

#include <functional>
#include <string>

namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::PlayKeptChanges
{
    using Apply = std::function<void( ECS::Entity& )>;

    /// Queue `apply` to run on the authored counterpart of `entity` once Stop has restored it. A second
    /// call with the same `key` for the same entity replaces the first.
    void Keep( const Common::UUID& entity, std::string key, Apply apply );

    /// Called by PlaySession::Stop after the restore: each kept change runs through
    /// Commands::MutateEntityUndoable on its entity, then the list is empty. An entity the restored world
    /// does not have (one Play spawned) drops its change, and the log says so.
    void ApplyTo( ::Desert::Core::Scene& restored );

    /// Called by PlaySession::Play: a new session starts with nothing kept.
    void Clear();
} // namespace Desert::Editor::PlayKeptChanges

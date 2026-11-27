#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>
#include <filesystem>

#include <Engine/Assets/Common.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

namespace Desert::ECS
{
    // Attaches a Lua script to an entity. The ScriptSystem loads the file and calls its OnStart()/OnUpdate(dt);
    // the script drives behavior through the bound API (self:move/jump/addYaw..., Input.*). See ScriptEngine.
    // A behavior unit, like a UE ActorComponent: one .lua file + its exposed properties + lifecycle flag.
    struct ScriptSlot
    {
        // The .lua file, as a ROOT-TAGGED KEY — the exact form Common::AssetHandle::StableKeyForPath
        // mints and Common::AssetHandle::PathForStableKey reads back, e.g.
        // "assets:Scripts/Examples/MoveAlongX.lua". NOT a path: never hand it to std::filesystem or to
        // an ifstream, ask ResolvedPath() below.
        //
        // WHY IT IS NOT A PATH ANY MORE (I9, scene schema v16). It used to hold the ROOTED spelling the
        // editor happened to be standing in — "Resources/Assets/Scripts/Examples/MoveAlongX.lua" — and a
        // rooted spelling does not survive packaging: a packaged game remaps ASSETS_PATH to
        // <package>/Assets/, so the stored string named a directory that does not exist there. Measured
        // on a mounted archive by I8: the stored spelling gave Exists=0 while the same file addressed
        // through the scripts root gave Exists=1. Every reference in a scene that goes through
        // MakeAssetResolver is an AssetHandle hashed from a root-tagged relative path and was already
        // immune; this was the only one that did not.
        //
        // IT WAS NOT THE ONLY ROOTED STRING IN A SCENE, and I10 finished the class one merge later.
        // `TextComponent.Font`, `UIText.Font`, `UIIcon.Icon` and `UIPanel.Video` also stored a path and
        // re-hashed it at load — the three service registries (FontService / IconService / VideoService)
        // are path-keyed through AssetHandle::FromCookedPath, so a scene could not store a handle for
        // them. They were safe for every value this repository ships (all 5 fonts and all 8 icons named
        // the ENGINE trees Resources/Fonts and Resources/Icons, which SetProjectRoot never remaps) and
        // broken for anything dropped in from the project's own assets tree, which both scan roots
        // accept. They are root-tagged keys now too, at scene v17, through one pair of functions in
        // Core/Serialize/ComponentRegistry.cpp. Every reference a `.desce` carries is now either an
        // AssetHandle resolved through MakeAssetResolver or a root-tagged key.
        //
        // WHY A KEY AND NOT A HANDLE, which is the other way this could have been fixed. An AssetHandle
        // IS the FNV-1a of exactly this string, so the hash carries no location the key does not — what
        // makes an asset reference survive the remap is the TAG plus the relative path, not the hashing.
        // The hash is one-way, so a handle only becomes a path again through the asset REGISTRY, and a
        // script has no registry entry, no loader and no payload the registry could hold: ScriptEngine
        // opens the file itself and ScriptSystem polls its mtime. Minting a ScriptAsset class purely to
        // invert a hash we would have computed from the string we already have is a table with one
        // reader — the "mirror with only one reader" anti-pattern — so the string is stored as it is.
        std::string ScriptKey;

        // The file ScriptKey names, on THIS host, in THIS project, right now. One place, so no call site
        // can forget the resolution and reintroduce the defect above; PathForStableKey refuses a
        // bare relative string by name (an empty path: no script), so no reader guesses a root for it.
        std::filesystem::path ResolvedPath() const
        {
            return Common::AssetHandle::PathForStableKey( ScriptKey );
        }

        // The values this entity OVERRIDES of the properties its script declares (Scripting::ScriptSchema): set in
        // Details, serialized, written into the script env before it runs and every frame after. A property not
        // listed runs on the script's own default — a new default in the script reaches every entity that never
        // overrode it, the way a UE instance holds only its delta from the class default.
        std::vector<Scripting::ScriptProperty> Properties;

        // Transient: the edit revision of Properties (UE's PostEditChangeProperty). Every writer of Properties
        // OUTSIDE the running script calls PropertiesEdited() — Details, a script swap; ScriptSystem pushes the
        // overrides into the running script only when this differs from the revision it pushed last
        // (ScriptEngine::SyncProperties), never property by property every frame. The SaveGame read-back does
        // NOT bump it: what it copies came out of the script, which already holds it.
        std::uint64_t PropertiesRevision = 0;
        void          PropertiesEdited() noexcept
        {
            ++PropertiesRevision;
        }

        bool Started = false; // transient: OnStart already called for this instance
        // Transient: the script's SaveGame property names (Scripting::ReadScriptSaveGameProperties), read
        // from the file each time the slot loads; ScriptSystem reads those properties back after the script ran.
        std::vector<std::string> SaveGameProperties;
        // Transient: why this slot is not running as written in the play world — its load error, else its last
        // OnUpdate error (ScriptEngine::SlotError); empty while it runs clean. Details shows it on the slot.
        std::string RuntimeError;
        // Transient: CallInEditor functions (Scripting::ScriptSchema::EditorFunctions) Details asked to run while
        // this slot plays; ScriptSystem calls each once on the running script in its next update.
        std::vector<std::string> PendingEditorCalls;
    };

    // One entity can run MANY scripts (EnTT allows only one component of a type per entity, so multiple
    // behaviors live as a LIST of slots inside this one component — the same composition UE gets from
    // multiple ActorComponents). Each slot is an independent sandbox (its own env + properties + lifecycle);
    // all slots share the same `self` entity. ScriptSystem ticks every slot; entity:call() broadcasts to all.
    /// A level sequence Event key's CallScript action (UE: an event endpoint on the bound actor): Lua function
    /// `Function` is called on every started slot with the key's name, by ScriptSystem in its next update.
    struct SequenceScriptCall
    {
        std::string Function;
        std::string EventName;
    };

    struct ScriptComponent
    {
        std::vector<ScriptSlot> Scripts;

        // Transient, like AnimationComponent::PendingNotifies: queued by LevelSequenceEntityHost::Fire, drained
        // (each call once) by ScriptSystem. Not serialized.
        std::vector<SequenceScriptCall> PendingSequenceCalls;
    };
} // namespace Desert::ECS

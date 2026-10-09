#pragma once

// INTERNAL scripting runtime — included ONLY by Engine/Scripting/*.cpp translation units.
// The scripts run on the Luau runtime (Luau/LuauRuntime: one VM, a sandboxed slot per script instance, the
// watchdog). This header defines what the engine's NATIVE modules share: the façade's Impl (the host every
// native reaches through its lua_State) and the module installers. The rest of the engine sees only
// ScriptEngine.hpp (PIMPL).
//
// BINDING ARCHITECTURE: the data of an entity is reached through reflection, generically — `self` is an
// entity object (LuauBinder: component/has/add/remove by the record key). What is NOT data — the engine's
// services (Log, Input, Timer, World, ...) and entity verbs (destroy, call, move, ...) — is a native module:
// one Register*Bindings translation unit each, installed on the main state before the sandbox freezes it.
// Adding a scripting domain = one Register*Bindings file and one line in Install (ScriptEngine.cpp).

#include "../ScriptEngine.hpp"

#include <Common/Core/Core.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Scripting/Luau/LuauBinder.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <VM/include/lualib.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Scripting
{
        // Maps script-friendly key names to engine key codes. Single characters cover the
        // whole alphabet + digits ("G", "5"); named keys cover the common gameplay set.
        inline std::optional<Common::KeyCode> KeyFromName( const std::string& name )
        {
            using K = Common::KeyCode;
            if ( name.size() == 1 )
            {
                const char c = name[0];
                if ( c >= 'A' && c <= 'Z' )
                    return static_cast<K>( c );
                if ( c >= 'a' && c <= 'z' )
                    return static_cast<K>( c - 'a' + 'A' );
                if ( c >= '0' && c <= '9' )
                    return static_cast<K>( c ); // KeyCode::D0..D9 == ASCII '0'..'9'
            }
            if ( name == "Space" ) return K::Space;
            if ( name == "Shift" || name == "LeftShift" ) return K::LeftShift;
            if ( name == "Ctrl" || name == "LeftControl" ) return K::LeftControl;
            if ( name == "Alt" || name == "LeftAlt" ) return K::LeftAlt;
            if ( name == "Tab" ) return K::Tab;
            if ( name == "Enter" ) return K::Enter;
            if ( name == "Escape" ) return K::Escape;
            if ( name == "Left" ) return K::Left;
            if ( name == "Right" ) return K::Right;
            if ( name == "Up" ) return K::Up;
            if ( name == "Down" ) return K::Down;
            return std::nullopt;
        }

        // The keys Input.wasPressed() edge-tracks each frame (NewInputFrame). Mirrors
        // KeyFromName's coverage: full alphabet + digits + the named gameplay keys.
        inline const std::vector<Common::KeyCode>& TrackedKeys()
        {
            static const std::vector<Common::KeyCode> keys = []
            {
                std::vector<Common::KeyCode> v;
                for ( int c = 'A'; c <= 'Z'; ++c )
                    v.push_back( static_cast<Common::KeyCode>( c ) );
                for ( int c = '0'; c <= '9'; ++c )
                    v.push_back( static_cast<Common::KeyCode>( c ) );
                using K = Common::KeyCode;
                for ( K k : { K::Space, K::LeftShift, K::LeftControl, K::LeftAlt, K::Tab, K::Enter,
                              K::Escape, K::Left, K::Right, K::Up, K::Down } )
                    v.push_back( k );
                return v;
            }();
            return keys;
        }

    struct ScriptEngine::Impl
    {
        Core::Scene*          Scene  = nullptr;
        Assets::AssetManager* Assets = nullptr;

        /// entt handle -> the runtime slot of each of its script SLOTS (index = ScriptComponent.Scripts index;
        /// 0 = that slot holds no running script).
        std::unordered_map<uint32_t, std::vector<LuauSlot>> Slots;

        float MouseDx = 0.0f;
        float MouseDy = 0.0f;

        // Input.wasPressed() edge state (down this frame & up last frame), refreshed by NewInputFrame().
        std::unordered_map<int, bool> KeyDownPrev;
        std::unordered_map<int, bool> KeyEdge;

        std::optional<bool> CursorLockRequest; // set by Input.lockCursor()/showCursor(), consumed by ScriptSystem

        // Last OnUpdate error per (entity, slot): a runtime error repeats every frame — log it ONCE until it
        // changes or the script reloads, so the Logs panel stays readable.
        std::unordered_map<uint64_t, std::string> LastUpdateError;

        // Timer.after(seconds, fn) scheduler. Each pending timer is OWNED by the (entity, slot) that scheduled
        // it, so a reload/release of that slot cancels its timers (a stale callback never fires into a replaced
        // sandbox). CurrentOwner is set right before the engine enters script code (top level, OnStart, OnUpdate,
        // a firing timer) — that is how Timer.after knows who schedules, and why a callback can re-arm itself.
        struct PendingTimer
        {
            uint64_t Owner     = 0; // SlotKey of the scheduling (entity, slot)
            float    Remaining = 0.0f;
            int      Fn        = LUA_NOREF; // the callback, pinned in the VM's registry
        };
        std::vector<PendingTimer> Timers;
        uint64_t                  CurrentOwner = 0;

        /// Shared script state of World.set/get/has (a registry table, alive as long as the VM).
        int WorldVars = LUA_NOREF;

        /// Entities a script destroyed: their slots are released by Settle, after the script call returns
        /// (a running slot's thread is never dropped under it).
        std::vector<uint32_t> PendingRelease;

        /// Calls (entity, slot)'s `function` when it defines one (success when it does not), then Settles.
        Common::BoolResultStr CallSlot( uint32_t entity, uint32_t slot, const char* function,
                                        std::span<const Reflection::Value> args );
        void Settle();
        void ReleaseEntity( uint32_t entity );

        /// Declared LAST: the VM's natives reach the fields above through Of(), so they outlive it.
        std::unique_ptr<LuauRuntime> Runtime;

        static uint64_t SlotKey( uint32_t entity, uint32_t slot )
        {
            return ( static_cast<uint64_t>( entity ) << 32 ) | slot;
        }

        /// The runtime slot of (entity, slot), or 0.
        LuauSlot SlotOf( uint32_t entity, uint32_t slot ) const
        {
            auto it = Slots.find( entity );
            return ( it == Slots.end() || slot >= it->second.size() ) ? 0 : it->second[slot];
        }

        /// Cancels the timers `drop` selects (their callbacks are unpinned).
        template <class Predicate> void DropTimers( Predicate drop )
        {
            std::erase_if( Timers,
                           [this, &drop]( const PendingTimer& timer )
                           {
                               if ( !drop( timer ) )
                                   return false;
                               Runtime->Unref( timer.Fn );
                               return true;
                           } );
        }

        /// The host a native runs for: stored in the VM's registry by Install.
        static Impl& Of( lua_State* L );

        /// The live entity at `index` as (scene, handle) — a Luau error when it is not an entity or is gone.
        entt::entity CheckEntity( lua_State* L, int index ) const
        {
            return LuauBinder::CheckEntity( L, index ).Entity;
        }
        entt::registry& Registry() const { return Scene->GetRegistry(); }
    };

    /// Reads the three numbers at `first..first+2` as a vector (a Luau `vector` at `first` is accepted too).
    glm::vec3 CheckVec3( lua_State* L, int first );

    // ── Native modules (one TU each; see the note above) ─────────────────────────────
    void RegisterLogBindings( lua_State* L );          // log(), Log.info/warn/error
    void RegisterEntityCoreBindings( lua_State* L );   // entity verbs: valid/name/destroy/call/transform/...
    void RegisterCharacterBindings( lua_State* L );    // move/jump/look/swim (gameplay)
    void RegisterMaterialBindings( lua_State* L );     // unified material protocol
    void RegisterInputBindings( lua_State* L );        // Input table
    void RegisterTimerBindings( lua_State* L );        // Timer.after scheduler
    void RegisterWorldBindings( lua_State* L );        // World table (find/spawn/raycast/vars)
    void RegisterAudioBindings( lua_State* L );        // Audio.play / Audio.stopAll
    void RegisterProjectBindings( lua_State* L );      // project.name/company
    void RegisterLevelBindings( lua_State* L );        // level.open (Core::OpenLevel)
    void RegisterAnimationBindings( lua_State* L );    // entity:setAnimParam/getAnimCurve/linkAnimLayers/...
    void RegisterUIBindings( lua_State* L );           // ui table (data store, collections, toasts)
    void RegisterLocalizationBindings( lua_State* L ); // loc table (text/plural/number/money/date/language)
} // namespace Desert::Scripting

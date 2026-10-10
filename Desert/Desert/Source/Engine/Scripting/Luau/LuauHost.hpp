#pragma once

// THE LUAU HOST — included ONLY by Engine/Scripting/Luau/*.cpp (and the suites that drive the host). The
// language-free Scripting module is ScriptEngine.hpp alone (PIMPL); this header is its Luau implementation:
// one VM, a sandboxed slot per script instance, the watchdog (LuauRuntime).
//
// BINDING ARCHITECTURE: what the ENGINE offers reaches Luau through reflection only — an entity's data by
// record key (self:component/has/add/remove), and every FUNCTION(ScriptCallable) of a reflected type
// (Engine/Libraries: Input, Audio, World.find, entity methods via ScriptMethod), bound by LuauBinder with no
// line per function. What is left here is about the language and the host itself (RegisterHostNatives:
// print-style log, Timer closures, World.set/get Luau values, entity valid/destroy/call) plus the ui.* and
// loc.* natives that wait for table-shaped Values (REMAINDER of SCR-PORT).

#include <Engine/Scripting/ScriptEngine.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
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
    struct ScriptEngine::Impl
    {
        Core::Scene*          Scene  = nullptr;
        Assets::AssetManager* Assets = nullptr;

        /// The world every entry into script code runs in (opened as a Core::WorldContext::Scope), so a
        /// reflected library function reaches the scene without a language type.
        Core::WorldContext Context;

        /// entt handle -> the runtime slot of each of its script SLOTS (index = ScriptComponent.Scripts index;
        /// 0 = that slot holds no running script).
        std::unordered_map<uint32_t, std::vector<LuauSlot>> Slots;

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
        void                  Settle();
        void                  ReleaseEntity( uint32_t entity );

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
        template <class Predicate>
        void DropTimers( Predicate drop )
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
        entt::registry& Registry() const
        {
            return Scene->GetRegistry();
        }
    };

    /// Reads the three numbers at `first..first+2` as a vector (a Luau `vector` at `first` is accepted too).
    glm::vec3 CheckVec3( lua_State* L, int first );

    // ── The host's natives (see the note above) ─────────────────────────────
    void RegisterHostNatives(
         lua_State* L ); // log(), Timer.after, World.set/get/has/raycast/cameraRay, valid/destroy/call
    void RegisterUIBindings( lua_State* L );           // ui table (data store, collections, toasts)
    void RegisterLocalizationBindings( lua_State* L ); // loc table (text/plural/number/money/date/language)
} // namespace Desert::Scripting

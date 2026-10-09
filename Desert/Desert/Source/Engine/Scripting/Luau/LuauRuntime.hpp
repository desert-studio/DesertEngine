#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Reflection/Value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Desert::Reflection
{
    struct TypeInfo;
}

namespace Desert::Scripting
{
    /// What one call into a script may spend before the runtime stops it.
    struct LuauLimits
    {
        /// THE WATCHDOG: a call (a script's top level, an event, a console line) still running after this long is
        /// stopped with an error at its next safepoint (loop back edge, call, return), so `while true do end`
        /// in OnUpdate fails that call instead of hanging the editor.
        std::chrono::milliseconds CallBudget{ 250 };

        /// The heap ONE SCRIPT (all the slots running it together) may hold. Accounted by a Luau memory
        /// category per script; a call that grows the script past it is stopped with an error.
        std::size_t ScriptMemoryBytes = std::size_t{ 64 } << 20U;
    };

    /// AN ENGINE OBJECT A SLOT SEES AS A GLOBAL (`self`, a component, a subsystem), bound through reflection:
    /// its reflected fields read and write as `obj.Field`, its FUNCTION(ScriptCallable) members call as
    /// `obj:Name(...)`. Nothing is written per type — the TypeInfo is the binding.
    ///
    /// Resolve answers "where is the instance NOW" on every access and returns nullptr once it is gone (a
    /// destroyed entity, a removed component): the script then gets an error naming it, never a dangling read.
    struct LuauBinding
    {
        std::string                 Name;
        const Reflection::TypeInfo* Type = nullptr;
        std::function<void*()>      Resolve;
    };

    using LuauSlot = std::uint32_t;

    /// THE LUAU RUNTIME — one VM, every script in its own sandbox, every call under the watchdog.
    ///
    /// The shape is Roblox's and Luau's own recommended embedding:
    ///  - the main state opens the libraries (Luau's `os` is only clock/date/difftime/time; there is no io,
    ///    dofile, loadfile; `loadstring` is removed here), binds the engine's static reflected functions, then
    ///    luaL_sandbox makes every library and global READ-ONLY, so one script cannot break another's `string`;
    ///  - a SLOT (one script instance on one object) is a thread with luaL_sandboxthread: its globals are its
    ///    own table that reads through to the frozen ones;
    ///  - a script is COMPILED ONCE: its bytecode is loaded into the main state as a prototype (native code
    ///    when the CPU supports it) and each slot runs a lua_clonefunction of it in its own globals. The cache
    ///    is keyed by the script's name and its exact source, so loading a hundred instances compiles once;
    ///  - HOT RELOAD recompiles a script and re-runs every slot of it in a fresh sandbox with the same
    ///    bindings; a reload that fails to compile or to run leaves the slots on the code they had.
    ///
    /// No Luau type appears here (PIMPL): the VM's headers stay in the runtime's own translation units.
    class LuauRuntime
    {
    public:
        explicit LuauRuntime( LuauLimits limits = {} );
        ~LuauRuntime();

        LuauRuntime( const LuauRuntime& )            = delete;
        LuauRuntime& operator=( const LuauRuntime& ) = delete;

        /// A new slot running `script` (its name: the file path, used as the chunk name in every error and as
        /// the cache key) from `source`, with `bindings` set as its globals before its top level runs.
        [[nodiscard]] Common::ResultStr<LuauSlot> Load( const std::string& script, const std::string& source,
                                                        std::vector<LuauBinding> bindings );

        /// Hot reload: `source` becomes the code of `script` and every slot running it re-runs from its top
        /// level in a fresh sandbox, with the bindings it was loaded with.
        [[nodiscard]] Common::BoolResultStr Reload( const std::string& script, const std::string& source );

        /// Drops the slot and everything only it referenced.
        void Release( LuauSlot slot );

        /// Whether the slot's globals hold a function under `function` (an engine event it answers).
        [[nodiscard]] bool Defines( LuauSlot slot, const char* function ) const;

        /// Calls the slot's global function `function` with `args`, under the watchdog. Refused when the slot or
        /// the function does not exist; a script error comes back with the script's own message.
        [[nodiscard]] Common::BoolResultStr Call( LuauSlot slot, const char* function,
                                                  std::span<const Reflection::Value> args = {} );

        /// THE CONSOLE (REPL): runs `code` as an expression first, then as a statement, in one sandbox that
        /// keeps its globals between lines. Everything printed and the expression's values land in `output`.
        [[nodiscard]] Common::BoolResultStr Eval( const std::string& code, std::string& output );

        /// The heap `script` holds across all its slots, in bytes (its memory category).
        [[nodiscard]] std::size_t ScriptMemory( const std::string& script ) const;

        /// How many times the compiler ran — the bytecode cache's evidence.
        [[nodiscard]] std::size_t CompileCount() const;

        struct Impl;

    private:
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Scripting

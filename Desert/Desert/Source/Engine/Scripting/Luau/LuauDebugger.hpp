#pragma once

#include <Engine/Scripting/ScriptDebugger.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace Desert::Scripting
{
    /// One loaded script of a runtime as the debugger patches it: the name it was loaded under (its chunk name,
    /// the file path), the registry reference of its prototype in the runtime's main state, its line count.
    struct LuauLoadedScript
    {
        std::string Script;
        int         Prototype = 0;
        int         Lines     = 0;
    };

    /// What a debuggable LuauRuntime hands the debugger when it registers.
    struct LuauDebugTarget
    {
        lua_State* L = nullptr; // the runtime's main state
        std::function<void( const std::function<void( const LuauLoadedScript& )>& )> ForEachScript;
        /// The call in flight was held at a stop for this long: the watchdog must not count it.
        std::function<void( std::chrono::steady_clock::duration )> Held;
    };

    /// THE LUAU SIDE OF THE SCRIPT DEBUGGER (ScriptDebugger.hpp).
    ///
    /// EVERY STOP IS A BREAK INSTRUCTION. Luau patches a line's first instruction to LOP_BREAK (lua_breakpoint)
    /// and calls the debugbreak callback when it runs; the callback stops synchronously (the world halts, the
    /// host's WhileStopped loop runs inside it). Stepping is not lua_singlestep: single-step only takes effect
    /// when the interpreter is ENTERED in that mode, and a script stopped at a breakpoint is already inside a
    /// non-stepping interpreter loop that cannot switch. So while a step (or a pause) is pending, every line of
    /// every loaded script is patched, and the callback decides by the call depth whether this line ends the
    /// step; the patches go back to the user's breakpoints at the stop.
    ///
    /// Call depth is (engine calls into script in flight, frames on the thread): a native that calls back into a
    /// script on another slot's thread is deeper, so Step Over passes over it like over any call.
    class LuauDebugger final : public IScriptDebugger
    {
    public:
        static LuauDebugger& Get();

        // ---- the runtime's side --------------------------------------------------------------------------
        void Register( LuauDebugTarget target );
        void Unregister( lua_State* L );

        /// A script of `L` was just compiled; its chunk is at the top of L's stack. Binds the breakpoints of its
        /// file (and the step patches while one is pending) before anything runs it.
        void Loaded( lua_State* L, const std::string& script, int lines );

        /// Whether a runtime may compile native code now (no line stops in native code).
        [[nodiscard]] bool NativeAllowed() const
        {
            return m_Host == nullptr;
        }

        /// Around every engine call into a script (LuauRuntime's Run).
        void EnterCall();
        void LeaveCall();

        // ---- IScriptDebugger -------------------------------------------------------------------------------
        void                                     Attach( IScriptDebugHost& host ) override;
        void                                     Detach() override;
        [[nodiscard]] bool                       Attached() const override;
        std::vector<DebugBreakpoint>             SetBreakpoints( const std::filesystem::path& file,
                                                                 std::span<const int>         lines ) override;
        [[nodiscard]] bool                       Stopped() const override;
        [[nodiscard]] std::vector<DebugFrame>    StackTrace() override;
        [[nodiscard]] std::vector<DebugScope>    Scopes( int frame ) override;
        [[nodiscard]] std::vector<DebugVariable> Variables( int reference ) override;
        void                                     Resume( DebugResume how ) override;
        void                                     Pause() override;
        void                                     Abort() override;

    private:
        enum class StepKind
        {
            None,
            Pause, // the next line anywhere
            In,
            Over,
            Out,
        };

        using Depth = std::pair<int, int>; // (engine calls in flight, frames on the thread)

        struct Step
        {
            StepKind   Kind   = StepKind::None;
            lua_State* Thread = nullptr;
            Depth      From{};
            int        Line = 0;
        };

        /// A group of variables a Reference names while stopped.
        struct Container
        {
            enum class Kind
            {
                Locals,
                Upvalues,
                Globals,
                Value, // a table pinned by Ref
            };
            Kind Of    = Kind::Locals;
            int  Level = 0;
            int  Ref   = 0;
        };

        /// A file's breakpoints: the lines asked for, with their ids.
        struct FileBreakpoints
        {
            std::vector<DebugBreakpoint> Lines;
        };

        static void OnBreakHook( lua_State* L, lua_Debug* ar );
        void        OnBreak( lua_State* L, int line );
        void        Stop( lua_State* L, int line, DebugStopReason reason, const std::string& description );

        /// The file a chunk name is, as the breakpoints key it (absolute, normalised).
        const std::string& FileOf( const std::string& script );

        /// The lines a script of `file` should have patched now: the user's, or every line while stepping.
        [[nodiscard]] std::set<int> WantedLines( const std::string& file, int lines ) const;

        /// Patches the chunk at `index` of L's stack from `patched` to `wanted`; returns the lines each user
        /// line bound to (requested line -> bound line).
        std::map<int, int> PatchChunk( lua_State* L, int index, std::set<int>& patched,
                                       const std::set<int>& wanted );

        /// Re-patches every loaded script of every runtime; rebinding records which breakpoint lines bound.
        void PatchAll();

        [[nodiscard]] Depth DepthOf( lua_State* L ) const;
        void                ReleaseContainers();
        int                 AddContainer( Container container );
        /// Describes the value at the top of the stopped thread's stack as `name` (a table gets a Reference).
        void PushVariable( std::vector<DebugVariable>& out, std::string name );

        IScriptDebugHost*                            m_Host = nullptr;
        std::vector<LuauDebugTarget>                 m_Targets;
        std::map<std::string, FileBreakpoints>       m_Breakpoints; // by FileOf key
        std::unordered_map<std::string, std::string> m_Files;       // chunk name -> FileOf key
        /// What is patched now, per (runtime, script) — a recompiled script starts unpatched (Loaded).
        std::map<std::pair<lua_State*, std::string>, std::set<int>> m_Patched;
        int                                                         m_NextBreakpointId = 1;

        int        m_CallDepth = 0;
        Step       m_Step;
        bool       m_Stopped       = false;
        bool       m_Aborting      = false;
        lua_State* m_StoppedThread = nullptr;
        int        m_StoppedLine   = 0;
        Depth      m_StoppedDepth{};

        std::vector<Container> m_Containers; // Reference = index + 1, valid while stopped
    };
} // namespace Desert::Scripting

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// THE SCRIPT DEBUGGER — what a debugger front end (the editor's DAP server, so VS Code attaches to a running
// editor or Play) asks of the scripting runtime, in no language's terms: breakpoints by file and line, the
// stopped call stack, its scopes and variables, continue / step / pause.
//
// The shape is UE's Blueprint debugger: a breakpoint HALTS THE WORLD on the game thread while the editor stays
// alive. The runtime's break callback calls IScriptDebugHost::WhileStopped in a loop until the front end resumes;
// the host services its client and pumps the window there, and nothing ticks a world. Everything here runs on
// the game thread: the runtime calls in from inside a script, the host calls back from inside WhileStopped.
namespace Desert::Scripting
{
    /// Why the runtime stopped.
    enum class DebugStopReason
    {
        Breakpoint,
        Step,
        Pause,
    };

    /// How the stopped script resumes.
    enum class DebugResume
    {
        Continue,
        StepOver, // the next line of this function (or of its caller, once it returns)
        StepIn,   // the next line run anywhere, a called function's first line included
        StepOut,  // the first line after this function returns
    };

    /// One breakpoint as the runtime holds it: the line asked for and the line it bound to (the next line with
    /// code; 0 while no loaded script has that file — it binds when one loads, and again after a hot reload).
    struct DebugBreakpoint
    {
        int  Id            = 0; // stable for the file and requested line, so a front end can match later events
        int  RequestedLine = 0;
        int  Line          = 0;
        bool Verified      = false;
    };

    /// One frame of the stopped stack, innermost first. File is empty for a native (C++) frame.
    struct DebugFrame
    {
        int                   Id = 0; // valid while stopped only
        std::string           Name;
        std::filesystem::path File;
        int                   Line = 0;
    };

    /// A named group of a frame's variables (Locals, Upvalues, Globals). Reference names it to Variables().
    struct DebugScope
    {
        std::string Name;
        int         Reference = 0;
    };

    /// A variable as text. Reference is non-zero when it has children (a table): pass it to Variables().
    struct DebugVariable
    {
        std::string Name;
        std::string Value;
        std::string Type;
        int         Reference = 0;
    };

    /// The front end, as the runtime reaches it.
    class IScriptDebugHost
    {
    public:
        virtual ~IScriptDebugHost() = default;

        /// The runtime stopped on the game thread; WhileStopped follows until the front end resumes.
        virtual void OnStopped( DebugStopReason reason, const std::string& description ) = 0;

        /// One turn of the stopped loop: service the client (it may call Resume / Abort / StackTrace ...), keep
        /// the window responsive. Must not tick a world or run a script.
        virtual void WhileStopped() = 0;

        /// A breakpoint of `file` bound (or moved) because a script with that file loaded or reloaded.
        virtual void OnBreakpointsBound( const std::filesystem::path&        file,
                                         const std::vector<DebugBreakpoint>& breakpoints ) = 0;
    };

    /// The runtime's side. One per process (ScriptDebugger()); every scripting runtime a world plays on reports
    /// to it, so one front end sees the editor world and the Play world alike.
    class IScriptDebugger
    {
    public:
        virtual ~IScriptDebugger() = default;

        /// Starts debugging: breakpoints take effect, natively compiled script code runs interpreted (native code
        /// has no line stops) until Detach.
        virtual void Attach( IScriptDebugHost& host ) = 0;

        /// Ends debugging: every breakpoint is removed, a stopped script resumes.
        virtual void Detach() = 0;

        [[nodiscard]] virtual bool Attached() const = 0;

        /// Replaces the breakpoints of `file` with `lines`; what each bound to, in the order of `lines`.
        virtual std::vector<DebugBreakpoint> SetBreakpoints( const std::filesystem::path& file,
                                                             std::span<const int>         lines ) = 0;

        /// Whether a script is stopped right now (the host is inside WhileStopped).
        [[nodiscard]] virtual bool Stopped() const = 0;

        /// The stopped stack, innermost first; empty when not stopped.
        [[nodiscard]] virtual std::vector<DebugFrame> StackTrace() = 0;

        /// The scopes of the stopped frame `frame` (DebugFrame::Id).
        [[nodiscard]] virtual std::vector<DebugScope> Scopes( int frame ) = 0;

        /// The variables a scope or a variable's Reference names; empty when the reference is stale.
        [[nodiscard]] virtual std::vector<DebugVariable> Variables( int reference ) = 0;

        /// Resumes the stopped script (`how`). While running: StepIn/StepOver/StepOut act as Pause.
        virtual void Resume( DebugResume how ) = 0;

        /// Stops at the next script line run anywhere.
        virtual void Pause() = 0;

        /// Resumes the stopped script with an error that ends its call, and stops nothing more until the
        /// engine's outermost script call returns — so the host can then end Play (a world is never torn down
        /// under a script that is still on the stack).
        virtual void Abort() = 0;
    };

    /// The process's script debugger.
    [[nodiscard]] IScriptDebugger& ScriptDebugger();
} // namespace Desert::Scripting

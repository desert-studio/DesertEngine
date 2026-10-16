#include <Engine/Scripting/Luau/LuauDebugger.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>

#include <CodeGen/include/Luau/CodeGen.h>
#include <VM/include/lua.h>
#include <VM/include/lualib.h>

namespace Desert::Scripting
{
    namespace
    {
        /// A file as both ends key it: absolute, symlinks resolved, '/' separated. The runtime's chunk names are
        /// the resolved script paths (ECS::ScriptSlot::ResolvedPath), often relative to the editor's working
        /// directory; a front end sends absolute paths.
        std::string Normalise( const std::filesystem::path& file )
        {
            std::error_code             error;
            const std::filesystem::path absolute = std::filesystem::absolute( file, error );
            if ( error )
                return file.generic_string();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical( absolute, error );
            return ( error ? absolute : canonical ).generic_string();
        }

        /// A chunk's script name: Luau keeps the chunk name LuauRuntime gave it, `@<script>`.
        std::string_view ScriptOfSource( const char* source )
        {
            if ( source == nullptr || source[0] != '@' )
                return {};
            return std::string_view( source + 1 );
        }

        std::string KeyName( lua_State* L, int index )
        {
            switch ( lua_type( L, index ) )
            {
                case LUA_TSTRING:
                {
                    std::size_t length = 0;
                    const char* text   = lua_tolstring( L, index, &length );
                    return { text, length };
                }
                case LUA_TNUMBER:
                    return std::format( "[{}]", lua_tonumber( L, index ) );
                case LUA_TBOOLEAN:
                    return lua_toboolean( L, index ) != 0 ? "[true]" : "[false]";
                default:
                    return std::format( "[{}: {}]", lua_typename( L, lua_type( L, index ) ),
                                        lua_topointer( L, index ) );
            }
        }
    } // namespace

    IScriptDebugger& ScriptDebugger()
    {
        return LuauDebugger::Get();
    }

    LuauDebugger& LuauDebugger::Get()
    {
        static LuauDebugger debugger;
        return debugger;
    }

    // ---- the runtime's side --------------------------------------------------------------------------------

    void LuauDebugger::Register( LuauDebugTarget target )
    {
        if ( m_Host != nullptr )
        {
            lua_callbacks( target.L )->debugbreak = &OnBreakHook;
            Luau::CodeGen::setNativeExecutionEnabled( target.L, false );
        }
        m_Targets.push_back( std::move( target ) );
    }

    void LuauDebugger::Unregister( lua_State* L )
    {
        std::erase_if( m_Targets, [L]( const LuauDebugTarget& target ) { return target.L == L; } );
        std::erase_if( m_Patched, [L]( const auto& entry ) { return entry.first.first == L; } );
    }

    void LuauDebugger::Loaded( lua_State* L, const std::string& script, int lines )
    {
        std::set<int>& patched = m_Patched[{ L, script }];
        patched.clear(); // a fresh prototype carries no patch
        if ( m_Host == nullptr )
            return;

        const std::string        file  = FileOf( script );
        const std::map<int, int> bound = PatchChunk( L, -1, patched, WantedLines( file, lines ) );
        auto                     found = m_Breakpoints.find( file );
        if ( found == m_Breakpoints.end() || found->second.Lines.empty() )
            return;
        for ( DebugBreakpoint& breakpoint : found->second.Lines )
        {
            const auto line = bound.find( breakpoint.RequestedLine );
            if ( line != bound.end() && line->second > 0 )
            {
                breakpoint.Line     = line->second;
                breakpoint.Verified = true;
            }
        }
        m_Host->OnBreakpointsBound( file, found->second.Lines );
    }

    void LuauDebugger::EnterCall()
    {
        ++m_CallDepth;
    }

    void LuauDebugger::LeaveCall()
    {
        --m_CallDepth;
        // The call a step started in has returned to the engine: the step ends at the next line any script runs
        // (the next event, the next frame's OnUpdate) — there is no script line "after" an engine call.
        const bool stepping =
             m_Step.Kind == StepKind::In || m_Step.Kind == StepKind::Over || m_Step.Kind == StepKind::Out;
        if ( stepping && m_CallDepth < m_Step.From.first )
            m_Step = Step{ .Kind = StepKind::Pause };
        if ( m_CallDepth == 0 )
            m_Aborting = false;
    }

    // ---- IScriptDebugger -----------------------------------------------------------------------------------

    void LuauDebugger::Attach( IScriptDebugHost& host )
    {
        m_Host = &host;
        for ( const LuauDebugTarget& target : m_Targets )
        {
            lua_callbacks( target.L )->debugbreak = &OnBreakHook;
            // Native code has no line stops: every native function runs interpreted while a debugger is attached.
            Luau::CodeGen::setNativeExecutionEnabled( target.L, false );
        }
        PatchAll();
    }

    void LuauDebugger::Detach()
    {
        m_Host    = nullptr;
        m_Step    = Step{};
        m_Stopped = false; // a stopped script resumes when its WhileStopped loop returns
        PatchAll();        // no host: no line is wanted, every patch is removed
        for ( const LuauDebugTarget& target : m_Targets )
        {
            lua_callbacks( target.L )->debugbreak = nullptr;
            Luau::CodeGen::setNativeExecutionEnabled( target.L, true );
        }
        for ( auto& [file, breakpoints] : m_Breakpoints )
            for ( DebugBreakpoint& breakpoint : breakpoints.Lines )
            {
                breakpoint.Line     = 0;
                breakpoint.Verified = false;
            }
    }

    bool LuauDebugger::Attached() const
    {
        return m_Host != nullptr;
    }

    std::vector<DebugBreakpoint> LuauDebugger::SetBreakpoints( const std::filesystem::path& file,
                                                               std::span<const int>         lines )
    {
        const std::string key = Normalise( file );
        FileBreakpoints&  old = m_Breakpoints[key];
        FileBreakpoints   fresh;
        for ( const int line : lines )
        {
            const auto kept = std::ranges::find( old.Lines, line, &DebugBreakpoint::RequestedLine );
            const int  id   = kept != old.Lines.end() ? kept->Id : m_NextBreakpointId++;
            fresh.Lines.push_back(
                 DebugBreakpoint{ .Id = id, .RequestedLine = line, .Line = 0, .Verified = false } );
        }
        old = std::move( fresh );
        PatchAll();
        return m_Breakpoints[key].Lines;
    }

    bool LuauDebugger::Stopped() const
    {
        return m_Stopped;
    }

    std::vector<DebugFrame> LuauDebugger::StackTrace()
    {
        std::vector<DebugFrame> frames;
        if ( !m_Stopped )
            return frames;
        lua_State* L = m_StoppedThread;
        for ( int level = 0;; ++level )
        {
            lua_Debug info{};
            if ( lua_getinfo( L, level, "sln", &info ) == 0 )
                break;
            DebugFrame frame;
            frame.Id                      = level + 1;
            const bool             native = info.what != nullptr && std::strcmp( info.what, "C" ) == 0;
            const bool             chunk  = info.what != nullptr && std::strcmp( info.what, "main" ) == 0;
            const std::string_view script = ScriptOfSource( info.source );
            if ( info.name != nullptr )
                frame.Name = native ? std::format( "[C] {}", info.name ) : info.name;
            else
                frame.Name = chunk ? "main chunk" : ( native ? "[C]" : "(anonymous function)" );
            if ( !native && !script.empty() )
            {
                frame.File = FileOf( std::string( script ) );
                frame.Line = info.currentline;
            }
            frames.push_back( std::move( frame ) );
        }
        return frames;
    }

    std::vector<DebugScope> LuauDebugger::Scopes( int frame )
    {
        if ( !m_Stopped )
            return {};
        const int level = frame - 1;
        lua_Debug info{};
        if ( level < 0 || lua_getinfo( m_StoppedThread, level, "s", &info ) == 0 ||
             ( info.what != nullptr && std::strcmp( info.what, "C" ) == 0 ) )
            return {};
        return {
             DebugScope{ .Name      = "Locals",
                         .Reference = AddContainer( { .Of = Container::Kind::Locals, .Level = level } ) },
             DebugScope{ .Name      = "Upvalues",
                         .Reference = AddContainer( { .Of = Container::Kind::Upvalues, .Level = level } ) },
             DebugScope{ .Name      = "Globals",
                         .Reference = AddContainer( { .Of = Container::Kind::Globals, .Level = level } ) },
        };
    }

    std::vector<DebugVariable> LuauDebugger::Variables( int reference )
    {
        std::vector<DebugVariable> out;
        if ( !m_Stopped || reference < 1 || reference > static_cast<int>( m_Containers.size() ) )
            return out;
        const Container container = m_Containers[static_cast<std::size_t>( reference - 1 )];
        lua_State*      L         = m_StoppedThread;
        lua_checkstack( L, 8 );
        const int base = lua_gettop( L );

        const auto table = [&]( int index )
        {
            lua_pushnil( L );
            while ( lua_next( L, index ) != 0 )
            {
                PushVariable( out, KeyName( L, -2 ) ); // the key stays a key: KeyName never converts it
                lua_pop( L, 1 );
            }
        };

        switch ( container.Of )
        {
            case Container::Kind::Locals:
                for ( int n = 1;; ++n )
                {
                    const char* name = lua_getlocal( L, container.Level, n );
                    if ( name == nullptr )
                        break;
                    if ( name[0] != '(' ) // the compiler's temporaries
                        PushVariable( out, name );
                    lua_pop( L, 1 );
                }
                break;
            case Container::Kind::Upvalues:
            {
                lua_Debug info{};
                if ( lua_getinfo( L, container.Level, "f", &info ) == 0 )
                    break;
                const int function = lua_gettop( L );
                for ( int n = 1;; ++n )
                {
                    const char* name = lua_getupvalue( L, function, n );
                    if ( name == nullptr )
                        break;
                    PushVariable( out, name[0] != '\0' ? std::string( name ) : std::format( "upvalue {}", n ) );
                    lua_pop( L, 1 );
                }
                break;
            }
            case Container::Kind::Globals:
            {
                lua_Debug info{};
                if ( lua_getinfo( L, container.Level, "f", &info ) == 0 )
                    break;
                // The function's environment is its slot's own globals table (luaL_sandboxthread); what it reads
                // through to is the frozen shared library set, which is not the script's state.
                lua_getfenv( L, -1 );
                if ( lua_istable( L, -1 ) )
                    table( lua_gettop( L ) );
                break;
            }
            case Container::Kind::Value:
                lua_getref( L, container.Ref );
                if ( lua_istable( L, -1 ) )
                    table( lua_gettop( L ) );
                break;
        }
        lua_settop( L, base );
        return out;
    }

    void LuauDebugger::Resume( DebugResume how )
    {
        if ( !m_Stopped )
        {
            if ( how != DebugResume::Continue )
                Pause();
            return;
        }
        if ( how == DebugResume::Continue )
            m_Step = Step{};
        else
            m_Step = Step{ .Kind   = how == DebugResume::StepIn     ? StepKind::In
                                     : how == DebugResume::StepOver ? StepKind::Over
                                                                    : StepKind::Out,
                           .Thread = m_StoppedThread,
                           .From   = m_StoppedDepth,
                           .Line   = m_StoppedLine };
        m_Stopped = false;
        PatchAll();
    }

    void LuauDebugger::Pause()
    {
        if ( m_Stopped || m_Host == nullptr )
            return;
        m_Step = Step{ .Kind = StepKind::Pause };
        PatchAll();
    }

    void LuauDebugger::Abort()
    {
        m_Step = Step{};
        if ( m_Stopped )
        {
            m_Aborting = true;
            m_Stopped  = false;
        }
        else if ( m_CallDepth > 0 )
        {
            m_Aborting = true;
        }
        PatchAll();
    }

    // ---- the stop ------------------------------------------------------------------------------------------

    void LuauDebugger::OnBreakHook( lua_State* L, lua_Debug* ar )
    {
        Get().OnBreak( L, ar->currentline );
    }

    void LuauDebugger::OnBreak( lua_State* L, int line )
    {
        if ( m_Host == nullptr || m_Stopped || m_Aborting )
            return;
        lua_Debug info{};
        if ( lua_getinfo( L, 0, "s", &info ) == 0 )
            return;
        const std::string_view script = ScriptOfSource( info.source );
        if ( script.empty() )
            return;
        const std::string& file = FileOf( std::string( script ) );

        if ( const auto found = m_Breakpoints.find( file ); found != m_Breakpoints.end() )
        {
            const bool user = std::ranges::any_of( found->second.Lines, [line]( const DebugBreakpoint& breakpoint )
                                                   { return breakpoint.Verified && breakpoint.Line == line; } );
            if ( user )
            {
                Stop( L, line, DebugStopReason::Breakpoint, std::format( "breakpoint at {}:{}", file, line ) );
                return;
            }
        }

        const Depth depth = DepthOf( L );
        bool        stop  = false;
        switch ( m_Step.Kind )
        {
            case StepKind::None:
                return; // a patch on a prototype a failed reload left running
            case StepKind::Pause:
                Stop( L, line, DebugStopReason::Pause, "paused" );
                return;
            case StepKind::In:
                stop = depth != m_Step.From || L != m_Step.Thread || line != m_Step.Line;
                break;
            case StepKind::Over:
                stop =
                     depth < m_Step.From || ( depth == m_Step.From && L == m_Step.Thread && line != m_Step.Line );
                break;
            case StepKind::Out:
                stop = depth < m_Step.From;
                break;
        }
        if ( stop )
            Stop( L, line, DebugStopReason::Step, "step" );
    }

    void LuauDebugger::Stop( lua_State* L, int line, DebugStopReason reason, const std::string& description )
    {
        m_Step          = Step{};
        m_Stopped       = true;
        m_StoppedThread = L;
        m_StoppedLine   = line;
        m_StoppedDepth  = DepthOf( L );
        PatchAll(); // back to the user's breakpoints while stopped

        const auto start = std::chrono::steady_clock::now();
        m_Host->OnStopped( reason, description );
        while ( m_Stopped && m_Host != nullptr )
            m_Host->WhileStopped();
        m_Stopped       = false;
        const auto held = std::chrono::steady_clock::now() - start;

        ReleaseContainers();
        if ( m_Host != nullptr )
            m_Host->OnResumed( held );
        m_StoppedThread       = nullptr;
        lua_State* const main = lua_mainthread( L );
        for ( const LuauDebugTarget& target : m_Targets )
            if ( target.L == main && target.Held )
                target.Held( held );

        if ( m_Aborting )
            luaL_errorL( L, "stopped by the script debugger" );
    }

    // ---- patches -------------------------------------------------------------------------------------------

    const std::string& LuauDebugger::FileOf( const std::string& script )
    {
        auto found = m_Files.find( script );
        if ( found == m_Files.end() )
            found = m_Files.emplace( script, Normalise( script ) ).first;
        return found->second;
    }

    std::set<int> LuauDebugger::WantedLines( const std::string& file, int lines ) const
    {
        std::set<int> wanted;
        if ( m_Host == nullptr )
            return wanted;
        if ( m_Step.Kind != StepKind::None && !m_Aborting )
        {
            for ( int line = 1; line <= lines; ++line )
                wanted.insert( line );
            return wanted;
        }
        if ( const auto found = m_Breakpoints.find( file ); found != m_Breakpoints.end() )
            for ( const DebugBreakpoint& breakpoint : found->second.Lines )
                wanted.insert( breakpoint.RequestedLine );
        return wanted;
    }

    std::map<int, int> LuauDebugger::PatchChunk( lua_State* L, int index, std::set<int>& patched,
                                                 const std::set<int>& wanted )
    {
        const int function = index < 0 ? lua_gettop( L ) + index + 1 : index;
        // Off first, then every wanted line on again: two requested lines can bind to one line with code (the
        // next one), and turning one of them off clears the instruction the other still wants.
        for ( const int line : patched )
            if ( !wanted.contains( line ) )
                (void)lua_breakpoint( L, function, line, 0 );
        std::map<int, int> bound;
        for ( const int line : wanted )
            bound[line] = lua_breakpoint( L, function, line, 1 );
        patched = wanted;
        return bound;
    }

    void LuauDebugger::PatchAll()
    {
        for ( auto& [file, breakpoints] : m_Breakpoints )
            for ( DebugBreakpoint& breakpoint : breakpoints.Lines )
            {
                breakpoint.Line     = 0;
                breakpoint.Verified = false;
            }

        for ( const LuauDebugTarget& target : m_Targets )
        {
            target.ForEachScript(
                 [&]( const LuauLoadedScript& script )
                 {
                     const std::string file = FileOf( script.Script );
                     lua_getref( target.L, script.Prototype );
                     const std::map<int, int> bound =
                          PatchChunk( target.L, -1, m_Patched[{ target.L, script.Script }],
                                      WantedLines( file, script.Lines ) );
                     lua_pop( target.L, 1 );
                     const auto found = m_Breakpoints.find( file );
                     if ( found == m_Breakpoints.end() )
                         return;
                     for ( DebugBreakpoint& breakpoint : found->second.Lines )
                     {
                         const auto line = bound.find( breakpoint.RequestedLine );
                         if ( line != bound.end() && line->second > 0 )
                         {
                             breakpoint.Line     = line->second;
                             breakpoint.Verified = true;
                         }
                     }
                 } );
        }
    }

    LuauDebugger::Depth LuauDebugger::DepthOf( lua_State* L ) const
    {
        return { m_CallDepth, lua_stackdepth( L ) };
    }

    void LuauDebugger::ReleaseContainers()
    {
        for ( const Container& container : m_Containers )
            if ( container.Of == Container::Kind::Value )
                lua_unref( m_StoppedThread, container.Ref );
        m_Containers.clear();
    }

    int LuauDebugger::AddContainer( Container container )
    {
        m_Containers.push_back( container );
        return static_cast<int>( m_Containers.size() );
    }

    void LuauDebugger::PushVariable( std::vector<DebugVariable>& out, std::string name )
    {
        lua_State*    L = m_StoppedThread;
        DebugVariable variable;
        variable.Name  = std::move( name );
        const int type = lua_type( L, -1 );
        variable.Type  = lua_typename( L, type );
        switch ( type )
        {
            case LUA_TNIL:
                variable.Value = "nil";
                break;
            case LUA_TBOOLEAN:
                variable.Value = lua_toboolean( L, -1 ) != 0 ? "true" : "false";
                break;
            case LUA_TNUMBER:
                variable.Value = std::format( "{}", lua_tonumber( L, -1 ) );
                break;
            case LUA_TVECTOR:
            {
                const float* vector = lua_tovector( L, -1 );
                variable.Value      = std::format( "vector({}, {}, {})", vector[0], vector[1], vector[2] );
                break;
            }
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                const char* text   = lua_tolstring( L, -1, &length );
                variable.Value     = std::format( "\"{}\"", std::string_view( text, length ) );
                break;
            }
            case LUA_TTABLE:
            {
                int entries = 0; // the array part AND the keyed part: lua_objlen counts only the first
                lua_pushnil( L );
                while ( lua_next( L, -2 ) != 0 )
                {
                    ++entries;
                    lua_pop( L, 1 );
                }
                variable.Value = std::format( "table ({} entries)", entries );
                variable.Reference =
                     AddContainer( { .Of = Container::Kind::Value, .Level = 0, .Ref = lua_ref( L, -1 ) } );
                break;
            }
            default:
                // A function, an engine object (userdata), a thread, a buffer: no __tostring is called — that
                // would run script code while the world is stopped inside a script.
                variable.Value = std::format( "{}: {}", variable.Type, lua_topointer( L, -1 ) );
                break;
        }
        out.push_back( std::move( variable ) );
    }
} // namespace Desert::Scripting

#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Scripting/Luau/LuauBinder.hpp>

#include <cstdlib>
#include <format>
#include <unordered_map>
#include <utility>

#include <CodeGen/include/luacodegen.h>
#include <Compiler/include/luacode.h>
#include <VM/include/lua.h>
#include <VM/include/lualib.h>

namespace Desert::Scripting
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        /// Luau numbers its memory categories 0..LUA_MEMORY_CATEGORIES-1; 0 is the engine's own (libraries,
        /// prototypes, the console). One category per SCRIPT, so the count is of distinct scripts, not slots.
        constexpr int kFirstScriptCategory = 1;
    } // namespace

    struct LuauRuntime::Impl
    {
        struct Script
        {
            std::string Source;
            int         Prototype = LUA_NOREF; // the loaded chunk in the main state, cloned by every slot
            int         Category  = 0;
        };

        struct Slot
        {
            std::string              Script;
            lua_State*               Thread    = nullptr;
            int                      ThreadRef = LUA_NOREF;
            std::vector<LuauBinding> Bindings;
        };

        /// The watchdog's state for the call in flight. Saved and restored around a call, so an engine
        /// function that calls back into a script keeps the outer call's deadline and category on return.
        struct Guard
        {
            bool              Armed = false;
            Clock::time_point Deadline;
            int               Category = 0;
        };

        LuauLimits                              Limits;
        lua_State*                              L          = nullptr;
        lua_State*                              Console    = nullptr;
        int                                     ConsoleRef = LUA_NOREF;
        bool                                    Native     = false;
        std::unordered_map<std::string, Script> Scripts;
        std::unordered_map<LuauSlot, Slot>      Slots;
        LuauSlot                                NextSlot     = 1;
        int                                     NextCategory = kFirstScriptCategory;
        std::size_t                             Compiles     = 0;
        Guard                                   Active;
        std::string*                            Capture = nullptr; // print() target while the console runs

        static Impl& Of( lua_State* state )
        {
            return *static_cast<Impl*>( lua_callbacks( state )->userdata );
        }

        static void Interrupt( lua_State* state, int gc )
        {
            if ( gc >= 0 ) // a GC step, not a safepoint of the script: nothing may be thrown here
                return;
            const Impl& impl = Of( state );
            if ( !impl.Active.Armed )
                return;
            if ( Clock::now() > impl.Active.Deadline )
                luaL_errorL( state, "stopped by the watchdog: the call ran longer than %lld ms",
                             static_cast<long long>( impl.Limits.CallBudget.count() ) );
            if ( impl.Active.Category != 0 &&
                 lua_totalbytes( state, impl.Active.Category ) > impl.Limits.ScriptMemoryBytes )
                luaL_errorL( state, "stopped: the script holds more than %zu bytes",
                             impl.Limits.ScriptMemoryBytes );
        }

        static int Print( lua_State* state )
        {
            Impl&       impl = Of( state );
            std::string line;
            const int   count = lua_gettop( state );
            for ( int i = 1; i <= count; ++i )
            {
                std::size_t length = 0;
                const char* text   = luaL_tolstring( state, i, &length );
                if ( i > 1 )
                    line += '\t';
                line.append( text, length );
                lua_pop( state, 1 );
            }
            if ( impl.Capture != nullptr )
            {
                *impl.Capture += line;
                *impl.Capture += '\n';
            }
            else
            {
                LOG_INFO( "[Luau] {}", line );
            }
            return 0;
        }

        /// Runs the function under the stack's top `nargs` arguments under the watchdog. Leaves `nresults`.
        Common::BoolResultStr Run( lua_State* thread, int nargs, int nresults, int category,
                                   const std::string& what )
        {
            const Guard outer = Active;
            Active = Guard{ .Armed = true, .Deadline = Clock::now() + Limits.CallBudget, .Category = category };
            const int status = lua_pcall( thread, nargs, nresults, 0 );
            Active           = outer;
            if ( status == LUA_OK )
                return Common::MakeSuccess( true );

            const char*       message = lua_tostring( thread, -1 );
            const std::string error =
                 std::format( "{}: {}", what, message != nullptr ? message : "(error is not a string)" );
            lua_pop( thread, 1 );
            return Common::MakeError<bool>( error );
        }

        /// Compiles `source` into a loaded chunk (and native code when supported); the function is left on L.
        Common::BoolResultStr Compile( const std::string& script, const std::string& source )
        {
            lua_CompileOptions options{};
            options.optimizationLevel = 1;
            options.debugLevel        = 1;
            std::size_t size          = 0;
            char*       bytecode      = luau_compile( source.data(), source.size(), &options, &size );
            ++Compiles;
            const std::string chunk  = "@" + script;
            const int         status = luau_load( L, chunk.c_str(), bytecode, size, 0 );
            std::free( bytecode );
            if ( status != 0 )
            {
                const std::string error = lua_tostring( L, -1 );
                lua_pop( L, 1 );
                return Common::MakeError<bool>( error );
            }
            if ( Native )
                luau_codegen_compile( L, -1 );
            return Common::MakeSuccess( true );
        }

        /// The script's prototype for `source`: the cached one when the source is the one it was built from.
        Common::ResultStr<Script*> Prototype( const std::string& script, const std::string& source )
        {
            auto found = Scripts.find( script );
            if ( found != Scripts.end() && found->second.Source == source )
                return Common::MakeSuccess( &found->second );

            if ( found == Scripts.end() && NextCategory >= LUA_MEMORY_CATEGORIES )
                return Common::MakeError<Script*>( std::format( "{}: the runtime accounts at most {} scripts",
                                                                script, LUA_MEMORY_CATEGORIES - 1 ) );
            if ( Common::BoolResultStr compiled = Compile( script, source ); !compiled.IsSuccess() )
                return Common::MakeError<Script*>( compiled.GetError() );

            const int prototype = lua_ref( L, -1 );
            lua_pop( L, 1 );
            if ( found == Scripts.end() )
                found = Scripts.emplace( script, Script{ .Category = NextCategory++ } ).first;
            else
                lua_unref( L, found->second.Prototype );
            found->second.Source    = source;
            found->second.Prototype = prototype;
            return Common::MakeSuccess( &found->second );
        }

        /// A fresh sandboxed thread running `script`'s top level with `bindings` as globals.
        Common::ResultStr<Slot> Start( const std::string& script, const Script& code,
                                       std::vector<LuauBinding> bindings )
        {
            lua_State* thread    = lua_newthread( L );
            const int  threadRef = lua_ref( L, -1 );
            lua_pop( L, 1 );
            luaL_sandboxthread( thread );
            lua_setmemcat( thread, code.Category );

            for ( const LuauBinding& binding : bindings )
            {
                LuauBinder::PushObject( thread, binding );
                lua_setglobal( thread, binding.Name.c_str() );
            }

            lua_getref( thread, code.Prototype );
            lua_clonefunction( thread, -1 ); // the clone's globals are the thread's sandbox
            lua_remove( thread, -2 );
            if ( Common::BoolResultStr ran = Run( thread, 0, 0, code.Category, script ); !ran.IsSuccess() )
            {
                lua_unref( L, threadRef );
                return Common::MakeError<Slot>( ran.GetError() );
            }
            return Common::MakeSuccess( Slot{
                 .Script = script, .Thread = thread, .ThreadRef = threadRef, .Bindings = std::move( bindings ) } );
        }
    };

    LuauRuntime::LuauRuntime( LuauLimits limits, const LuauInstall& install ) : m_Impl( std::make_unique<Impl>() )
    {
        Impl& impl  = *m_Impl;
        impl.Limits = limits;
        impl.L      = luaL_newstate();

        lua_Callbacks* callbacks = lua_callbacks( impl.L );
        callbacks->userdata      = &impl;
        callbacks->interrupt     = &Impl::Interrupt;

        impl.Native = luau_codegen_supported() != 0;
        if ( impl.Native )
            luau_codegen_create( impl.L );

        luaL_openlibs( impl.L );
        lua_pushnil( impl.L );
        lua_setglobal( impl.L, "loadstring" ); // compiling text at run time is the packer's job, not a script's
        lua_pushcfunction( impl.L, &Impl::Print, "print" );
        lua_setglobal( impl.L, "print" );
        LuauBinder::Install( impl.L );
        if ( install )
            install( impl.L );
        luaL_sandbox( impl.L );

        impl.Console    = lua_newthread( impl.L );
        impl.ConsoleRef = lua_ref( impl.L, -1 );
        lua_pop( impl.L, 1 );
        luaL_sandboxthread( impl.Console );
    }

    LuauRuntime::~LuauRuntime()
    {
        lua_close( m_Impl->L );
    }

    Common::ResultStr<LuauSlot> LuauRuntime::Load( const std::string& script, const std::string& source,
                                                   std::vector<LuauBinding> bindings )
    {
        Impl& impl = *m_Impl;
        // Two shapes, nothing in between (LuauRuntime.hpp LuauBinding): an object (Type + Resolve) or an entity
        // (Entity resolver, no Type / Resolve).
        for ( const LuauBinding& binding : bindings )
        {
            const bool object = binding.Type != nullptr && binding.Resolve && !binding.Entity;
            const bool entity = binding.Type == nullptr && !binding.Resolve && binding.Entity;
            if ( !object && !entity )
                return Common::MakeError<LuauSlot>( std::format(
                     "{}: binding '{}' is neither an object (reflected type + resolver) nor an entity (entity "
                     "resolver only)",
                     script, binding.Name ) );
        }

        Common::ResultStr<Impl::Script*> code = impl.Prototype( script, source );
        if ( !code.IsSuccess() )
            return Common::MakeError<LuauSlot>( code.GetError() );
        Common::ResultStr<Impl::Slot> slot = impl.Start( script, *code.GetValue(), std::move( bindings ) );
        if ( !slot.IsSuccess() )
            return Common::MakeError<LuauSlot>( slot.GetError() );

        const LuauSlot id = impl.NextSlot++;
        impl.Slots.emplace( id, slot.ExtractValue() );
        return Common::MakeSuccess( LuauSlot{ id } );
    }

    Common::BoolResultStr LuauRuntime::Reload( const std::string& script, const std::string& source )
    {
        Impl& impl = *m_Impl;
        if ( !impl.Scripts.contains( script ) )
            return Common::MakeError<bool>( std::format( "{}: no slot runs this script", script ) );
        Common::ResultStr<Impl::Script*> code = impl.Prototype( script, source );
        if ( !code.IsSuccess() )
            return Common::MakeError<bool>( code.GetError() );

        std::string failures;
        for ( auto& [id, slot] : impl.Slots )
        {
            if ( slot.Script != script )
                continue;
            Common::ResultStr<Impl::Slot> fresh = impl.Start( script, *code.GetValue(), slot.Bindings );
            if ( !fresh.IsSuccess() )
            {
                failures += std::format( "{}slot {}: {}", failures.empty() ? "" : "; ", id, fresh.GetError() );
                continue;
            }
            lua_unref( impl.L, slot.ThreadRef );
            slot = fresh.ExtractValue();
        }
        if ( !failures.empty() )
            return Common::MakeError<bool>(
                 std::format( "{}: reloaded, but these slots kept the old code: {}", script, failures ) );
        return Common::MakeSuccess( true );
    }

    void LuauRuntime::Release( LuauSlot slot )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return;
        lua_unref( impl.L, found->second.ThreadRef );
        impl.Slots.erase( found );
    }

    bool LuauRuntime::Defines( LuauSlot slot, const char* function ) const
    {
        const Impl& impl  = *m_Impl;
        auto        found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return false;
        lua_State* thread = found->second.Thread;
        lua_getglobal( thread, function );
        const bool defined = lua_isfunction( thread, -1 );
        lua_pop( thread, 1 );
        return defined;
    }

    Common::BoolResultStr LuauRuntime::Call( LuauSlot slot, const char* function,
                                             std::span<const Reflection::Value> args )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return Common::MakeError<bool>( std::format( "slot {} does not exist", slot ) );
        const Impl::Slot& target = found->second;
        lua_State*        thread = target.Thread;

        lua_getglobal( thread, function );
        if ( !lua_isfunction( thread, -1 ) )
        {
            lua_pop( thread, 1 );
            return Common::MakeError<bool>(
                 std::format( "{}: defines no function '{}'", target.Script, function ) );
        }
        for ( const Reflection::Value& arg : args )
            LuauBinder::PushValue( thread, arg );
        return impl.Run( thread, static_cast<int>( args.size() ), 0, impl.Scripts.at( target.Script ).Category,
                         std::format( "{} {}", target.Script, function ) );
    }

    Common::BoolResultStr LuauRuntime::CallWithEntity( LuauSlot slot, const char* function,
                                                       entt::registry& registry, entt::entity entity )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return Common::MakeError<bool>( std::format( "slot {} does not exist", slot ) );
        const Impl::Slot& target = found->second;
        lua_State*        thread = target.Thread;

        lua_getglobal( thread, function );
        if ( !lua_isfunction( thread, -1 ) )
        {
            lua_pop( thread, 1 );
            return Common::MakeError<bool>(
                 std::format( "{}: defines no function '{}'", target.Script, function ) );
        }
        LuauBinder::PushEntity( thread, registry, entity );
        return impl.Run( thread, 1, 0, impl.Scripts.at( target.Script ).Category,
                         std::format( "{} {}", target.Script, function ) );
    }

    Common::BoolResultStr LuauRuntime::CallFrom( LuauSlot slot, const char* function, lua_State* from, int first,
                                                 int count )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return Common::MakeError<bool>( std::format( "slot {} does not exist", slot ) );
        const Impl::Slot& target = found->second;
        lua_State*        thread = target.Thread;

        lua_getglobal( thread, function );
        if ( !lua_isfunction( thread, -1 ) )
        {
            lua_pop( thread, 1 );
            return Common::MakeError<bool>(
                 std::format( "{}: defines no function '{}'", target.Script, function ) );
        }
        for ( int i = 0; i < count; ++i )
            lua_xpush( from, thread, first + i );
        return impl.Run( thread, count, 0, impl.Scripts.at( target.Script ).Category,
                         std::format( "{} {}", target.Script, function ) );
    }

    Common::BoolResultStr LuauRuntime::CallRef( LuauSlot slot, int function )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return Common::MakeError<bool>( std::format( "slot {} does not exist", slot ) );
        const Impl::Slot& target = found->second;
        lua_getref( target.Thread, function );
        if ( !lua_isfunction( target.Thread, -1 ) )
        {
            lua_pop( target.Thread, 1 );
            return Common::MakeError<bool>( std::format( "{}: the callback is gone", target.Script ) );
        }
        return impl.Run( target.Thread, 0, 0, impl.Scripts.at( target.Script ).Category,
                         std::format( "{} callback", target.Script ) );
    }

    void LuauRuntime::Unref( int reference )
    {
        lua_unref( m_Impl->L, reference );
    }

    void LuauRuntime::SetTableField( LuauSlot slot, const char* table, const std::string& key,
                                     const Reflection::Value& value )
    {
        Impl& impl  = *m_Impl;
        auto  found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return;
        lua_State* thread = found->second.Thread;
        lua_getglobal( thread, table );
        if ( !lua_istable( thread, -1 ) )
        {
            lua_pop( thread, 1 );
            lua_newtable( thread );
            lua_pushvalue( thread, -1 );
            lua_setglobal( thread, table );
        }
        LuauBinder::PushValue( thread, value );
        lua_setfield( thread, -2, key.c_str() );
        lua_pop( thread, 1 );
    }

    std::vector<LuauTableEntry> LuauRuntime::ReadTable( LuauSlot slot, const char* table ) const
    {
        std::vector<LuauTableEntry> entries;
        const Impl&                 impl  = *m_Impl;
        auto                        found = impl.Slots.find( slot );
        if ( found == impl.Slots.end() )
            return entries;
        lua_State* thread = found->second.Thread;
        lua_getglobal( thread, table );
        if ( lua_istable( thread, -1 ) )
        {
            lua_pushnil( thread );
            while ( lua_next( thread, -2 ) != 0 )
            {
                if ( lua_type( thread, -2 ) == LUA_TSTRING )
                {
                    std::string key = lua_tostring( thread, -2 );
                    switch ( lua_type( thread, -1 ) )
                    {
                        case LUA_TBOOLEAN:
                            entries.push_back( { std::move( key ), Reflection::Value::Bool( lua_toboolean( thread, -1 ) != 0 ) } );
                            break;
                        case LUA_TNUMBER:
                            entries.push_back( { std::move( key ), Reflection::Value::Double( lua_tonumber( thread, -1 ) ) } );
                            break;
                        case LUA_TSTRING:
                            entries.push_back( { std::move( key ), Reflection::Value::String( lua_tostring( thread, -1 ) ) } );
                            break;
                        default: break; // a table / function default is not an editor property
                    }
                }
                lua_pop( thread, 1 );
            }
        }
        lua_pop( thread, 1 );
        return entries;
    }

    Common::BoolResultStr LuauRuntime::Eval( const std::string& code, std::string& output )
    {
        Impl&      impl    = *m_Impl;
        lua_State* console = impl.Console;

        // An expression first ("1 + 2" prints 3), then the line as a statement ("x = 1").
        lua_CompileOptions options{};
        options.debugLevel = 1;
        bool        loaded = false;
        std::string error;
        for ( const std::string& text : { "return " + code, code } )
        {
            std::size_t size     = 0;
            char*       bytecode = luau_compile( text.data(), text.size(), &options, &size );
            ++impl.Compiles;
            const int status = luau_load( console, "=console", bytecode, size, 0 );
            std::free( bytecode );
            if ( status == 0 )
            {
                loaded = true;
                break;
            }
            error = lua_tostring( console, -1 );
            lua_pop( console, 1 );
        }
        if ( !loaded )
            return Common::MakeError<bool>( error );

        const int base            = lua_gettop( console ) - 1;
        impl.Capture              = &output;
        Common::BoolResultStr ran = impl.Run( console, 0, LUA_MULTRET, 0, "console" );
        impl.Capture              = nullptr;
        if ( !ran.IsSuccess() )
            return ran;

        const int results = lua_gettop( console ) - base;
        for ( int i = 1; i <= results; ++i )
        {
            std::size_t length = 0;
            const char* text   = luaL_tolstring( console, base + i, &length );
            if ( i > 1 )
                output += '\t';
            output.append( text, length );
            lua_pop( console, 1 );
        }
        if ( results > 0 )
            output += '\n';
        lua_settop( console, base );
        return Common::MakeSuccess( true );
    }

    std::size_t LuauRuntime::ScriptMemory( const std::string& script ) const
    {
        auto found = m_Impl->Scripts.find( script );
        return found == m_Impl->Scripts.end() ? 0 : lua_totalbytes( m_Impl->L, found->second.Category );
    }

    std::size_t LuauRuntime::CompileCount() const
    {
        return m_Impl->Compiles;
    }
} // namespace Desert::Scripting

#include "Internal/ScriptRuntime.hpp"

#include <Common/Utilities/FileSystem.hpp>
#include <filesystem>

namespace Desert::Scripting
{
    ScriptEngine::ScriptEngine( Core::Scene* scene, Assets::AssetManager* assetManager )
         : m_Impl( std::make_unique<Impl>() )
    {
        m_Impl->Scene  = scene;
        m_Impl->Assets = assetManager;

        m_Impl->Lua.open_libraries( sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                                    sol::lib::os );

        // Modular bindings: the core owns the VM; every domain registers its own API from its
        // own translation unit (see Internal/ScriptRuntime.hpp for the architecture note).
        RegisterLogBindings( *m_Impl );
        RegisterEntityCoreBindings( *m_Impl );
        RegisterCharacterBindings( *m_Impl );
        RegisterMaterialBindings( *m_Impl );
        RegisterInputBindings( *m_Impl );
        RegisterTimerBindings( *m_Impl );
        RegisterWorldBindings( *m_Impl );
        RegisterReflectionBindings( *m_Impl ); // after EntityCore: extends the Entity usertype
        RegisterAudioBindings( *m_Impl );
        RegisterAnimationBindings( *m_Impl ); // after EntityCore: extends the Entity usertype
        RegisterUIBindings( *m_Impl );
        RegisterLocalizationBindings( *m_Impl );
    }

    ScriptEngine::~ScriptEngine() = default;

    Common::BoolResultStr ScriptEngine::RunString( const std::string& code )
    {
        sol::protected_function_result result = m_Impl->Lua.safe_script( code, sol::script_pass_on_error );
        if ( !result.valid() )
        {
            sol::error err = result;
            return Common::MakeError( err.what() );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScriptEngine::EvalToString( const std::string& code, std::string& output )
    {
        auto& lua = m_Impl->Lua;
        output.clear();

        // Redirect print() into a table for the duration of this eval (this is the console's OWN VM, and
        // print is restored right after).
        lua.safe_script( "__repl_out = {}; __repl_old_print = print; "
                         "function print(...) local t = {} for i = 1, select('#', ...) do "
                         "t[i] = tostring(select(i, ...)) end "
                         "__repl_out[#__repl_out + 1] = table.concat(t, '\\t') end",
                         sol::script_pass_on_error );

        // Try as an expression first ("2+2", "World.count()") so its value is shown; fall back to running
        // it as a statement ("x = 5", "for ...") only when the expression form does not COMPILE — so a
        // runtime error in a valid expression is reported once, not run twice.
        sol::protected_function_result r;
        bool                           ranAsExpr = false;
        if ( sol::load_result exprChunk = lua.load( "return (" + code + ")" ); exprChunk.valid() )
        {
            sol::protected_function fn = exprChunk;
            r                          = fn();
            ranAsExpr                  = true;
        }
        else
        {
            r = lua.safe_script( code, sol::script_pass_on_error );
        }

        lua.safe_script( "print = __repl_old_print", sol::script_pass_on_error );

        // Collect captured print() lines (array part, in order).
        if ( sol::table out = lua["__repl_out"]; out.valid() )
        {
            const std::size_t n = out.size();
            for ( std::size_t i = 1; i <= n; ++i )
            {
                output += out.get<std::string>( i );
                output += '\n';
            }
        }

        if ( !r.valid() )
        {
            sol::error err = r;
            return Common::MakeError( err.what() );
        }

        // Append the expression's value, if it produced one.
        if ( ranAsExpr && r.return_count() > 0 )
        {
            sol::object v = r[0];
            if ( v.valid() && v.get_type() != sol::type::lua_nil )
            {
                const std::string s = lua["tostring"]( v );
                output += s;
                output += '\n';
            }
        }
        return BOOLSUCCESS;
    }

    // Returns the env for (entity, slot), or nullptr if not loaded. Grows the slot vector on demand when
    // `create` is set (used by LoadEntityScript so slots can be (re)loaded in any order).
    static sol::environment* SlotEnv( EnvMap& envs, uint32_t entity, uint32_t slot, bool create )
    {
        auto it = envs.find( entity );
        if ( it == envs.end() )
        {
            if ( !create )
                return nullptr;
            it = envs.emplace( entity, std::vector<sol::environment>{} ).first;
        }
        if ( slot >= it->second.size() )
        {
            if ( !create )
                return nullptr;
            it->second.resize( slot + 1, sol::environment{} );
        }
        return &it->second[slot];
    }

    Common::BoolResultStr ScriptEngine::LoadEntityScript( uint32_t entity, uint32_t slot,
                                                          const std::string& path )
    {
        sol::environment env( m_Impl->Lua, sol::create, m_Impl->Lua.globals() );
        env["self"] = m_Impl->MakeEntity( static_cast<entt::entity>( entity ) );

        // Disk scripts load via sol's file path (dev / hot-reload); a packaged game reads the source
        // out of the mounted .dpak and loads it as a string chunk. A path the VFS does not know
        // either — or knows but cannot read — becomes a Lua error() chunk, so the failure surfaces
        // through the same channel as any script error below.
        std::string packedSource;
        if ( !std::filesystem::exists( path ) )
        {
            packedSource = std::string( "error('script not found: " ).append( path ).append( "')" );
            if ( Common::Utils::FileSystem::Exists( path ) )
            {
                if ( auto packed = Common::Utils::FileSystem::ReadFileContent( path ); packed )
                    packedSource = packed.ExtractValue();
            }
        }
        sol::protected_function_result r =
             std::filesystem::exists( path )
                  ? m_Impl->Lua.safe_script_file( path, env, sol::script_pass_on_error )
                  : m_Impl->Lua.safe_script( packedSource, env, sol::script_pass_on_error );
        if ( !r.valid() )
        {
            sol::error err = r;
            return Common::MakeError( err.what() );
        }

        *SlotEnv( m_Impl->Envs, entity, slot, /*create*/ true ) = std::move( env );
        const uint64_t key = Impl::SlotKey( entity, slot );
        m_Impl->LastUpdateError.erase( key ); // fresh env -> fresh error state
        // Fresh env -> the OLD env's pending timers must not fire into it (hot-reload safety).
        std::erase_if( m_Impl->Timers, [key]( const Impl::PendingTimer& t ) { return t.Owner == key; } );
        return BOOLSUCCESS;
    }

    void ScriptEngine::CallStart( uint32_t entity, uint32_t slot )
    {
        sol::environment* env = SlotEnv( m_Impl->Envs, entity, slot, false );
        if ( !env )
            return;
        sol::protected_function fn = ( *env )["OnStart"];
        if ( !fn.valid() )
            return;
        m_Impl->CurrentOwner             = Impl::SlotKey( entity, slot ); // Timer.after ownership
        sol::protected_function_result r = fn();
        if ( !r.valid() )
        {
            sol::error err = r;
            LOG_ERROR( "[Lua] OnStart error: {}", err.what() );
        }
    }

    void ScriptEngine::CallAnimationNotify( uint32_t entity, uint32_t slot, const std::string& name )
    {
        sol::environment* env = SlotEnv( m_Impl->Envs, entity, slot, false );
        if ( !env )
            return;
        sol::protected_function fn = ( *env )["OnAnimationNotify"];
        if ( !fn.valid() )
            return;
        m_Impl->CurrentOwner             = Impl::SlotKey( entity, slot ); // Timer.after ownership
        sol::protected_function_result r = fn( name );
        if ( !r.valid() )
        {
            sol::error err = r;
            LOG_ERROR( "[Lua] OnAnimationNotify error: {}", err.what() );
        }
    }

    void ScriptEngine::BroadcastUIMessage( const std::string& message )
    {
        for ( auto& [entity, slots] : m_Impl->Envs )
        {
            for ( uint32_t slot = 0; slot < static_cast<uint32_t>( slots.size() ); ++slot )
            {
                sol::protected_function fn = slots[slot]["OnUIMessage"];
                if ( !fn.valid() )
                    continue;
                m_Impl->CurrentOwner             = Impl::SlotKey( entity, slot ); // Timer.after ownership
                sol::protected_function_result r = fn( message );
                if ( !r.valid() )
                {
                    sol::error err = r;
                    LOG_ERROR( "[Lua] OnUIMessage error: {}", err.what() );
                }
            }
        }
    }

    void ScriptEngine::CallUpdate( uint32_t entity, uint32_t slot, float dt )
    {
        sol::environment* env = SlotEnv( m_Impl->Envs, entity, slot, false );
        if ( !env )
            return;
        sol::protected_function fn = ( *env )["OnUpdate"];
        if ( !fn.valid() )
            return;
        const uint64_t key    = Impl::SlotKey( entity, slot );
        m_Impl->CurrentOwner  = key; // Timer.after ownership
        sol::protected_function_result r = fn( dt );
        if ( !r.valid() )
        {
            sol::error        err  = r;
            const std::string what = err.what();
            // OnUpdate runs every frame — report a given error once, not 60x/sec.
            if ( m_Impl->LastUpdateError[key] != what )
            {
                m_Impl->LastUpdateError[key] = what;
                LOG_ERROR( "[Lua] OnUpdate error: {}", what );
            }
        }
        else
        {
            m_Impl->LastUpdateError.erase( key );
        }
    }

    void ScriptEngine::ApplyProperties( uint32_t entity, uint32_t slot,
                                        const std::vector<ScriptProperty>& props )
    {
        sol::environment* env = SlotEnv( m_Impl->Envs, entity, slot, false );
        if ( !env )
            return;

        // Ensure the env has a `Properties` table (the script usually declares one, but be safe).
        sol::object existing = ( *env )["Properties"];
        if ( !existing.is<sol::table>() )
            ( *env )["Properties"] = m_Impl->Lua.create_table();
        sol::table t = ( *env )["Properties"];

        for ( const auto& p : props )
        {
            switch ( p.Type )
            {
                case PropertyType::Number: t[p.Name] = p.Number; break;
                case PropertyType::Bool:   t[p.Name] = p.Bool; break;
                case PropertyType::String: t[p.Name] = p.Str; break;
            }
        }
    }

    void ScriptEngine::Release( uint32_t entity )
    {
        m_Impl->Envs.erase( entity );
        std::erase_if( m_Impl->Timers, [entity]( const Impl::PendingTimer& t )
                       { return static_cast<uint32_t>( t.Owner >> 32 ) == entity; } );
    }

    void ScriptEngine::TrimSlots( uint32_t entity, uint32_t count )
    {
        auto it = m_Impl->Envs.find( entity );
        if ( it != m_Impl->Envs.end() && it->second.size() > count )
            it->second.resize( count );
        std::erase_if( m_Impl->Timers, [entity, count]( const Impl::PendingTimer& t )
                       { return static_cast<uint32_t>( t.Owner >> 32 ) == entity &&
                                static_cast<uint32_t>( t.Owner & 0xFFFFFFFFu ) >= count; } );
    }

    void ScriptEngine::TickTimers( float dt )
    {
        // Two-phase so a firing callback can safely schedule new timers (Timer.after re-arm):
        // extract everything due first, then invoke — new pushes land in m_Impl->Timers untouched.
        std::vector<Impl::PendingTimer> due;
        for ( auto it = m_Impl->Timers.begin(); it != m_Impl->Timers.end(); )
        {
            it->Remaining -= dt;
            if ( it->Remaining <= 0.0f )
            {
                due.push_back( std::move( *it ) );
                it = m_Impl->Timers.erase( it );
            }
            else
                ++it;
        }
        for ( auto& t : due )
        {
            m_Impl->CurrentOwner             = t.Owner; // a re-arm inherits the same (entity, slot)
            sol::protected_function_result r = t.Fn();
            if ( !r.valid() )
            {
                sol::error err = r;
                LOG_ERROR( "[Lua] Timer.after error: {}", err.what() );
            }
        }
    }

    std::vector<ScriptProperty> ReadScriptProperties( const std::string& path )
    {
        std::vector<ScriptProperty> out;

        // Throwaway state: we only need to read the top-level `Properties` table. base lib is enough — the
        // file's top level just sets locals / Properties / defines functions (no engine calls at load time).
        sol::state lua;
        lua.open_libraries( sol::lib::base, sol::lib::math );
        // An unreadable/absent script stays an empty chunk: this probe only harvests Properties, so
        // "no properties" is the correct answer for a script that cannot run.
        std::string packedSource;
        if ( !std::filesystem::exists( path ) && Common::Utils::FileSystem::Exists( path ) )
        {
            if ( auto packed = Common::Utils::FileSystem::ReadFileContent( path ); packed )
                packedSource = packed.ExtractValue();
        }
        sol::protected_function_result r = std::filesystem::exists( path )
                                                ? lua.safe_script_file( path, sol::script_pass_on_error )
                                                : lua.safe_script( packedSource, sol::script_pass_on_error );
        if ( !r.valid() )
            return out;

        sol::object propsObj = lua["Properties"];
        if ( !propsObj.is<sol::table>() )
            return out;

        sol::table props = propsObj.as<sol::table>();
        for ( const auto& kv : props )
        {
            if ( kv.first.get_type() != sol::type::string )
                continue;
            const std::string name = kv.first.as<std::string>();
            const sol::object value = kv.second;

            ScriptProperty p;
            p.Name = name;
            if ( value.is<bool>() ) // check bool BEFORE number (distinct Lua types)
            {
                p.Type = PropertyType::Bool;
                p.Bool = value.as<bool>();
            }
            else if ( value.is<double>() )
            {
                p.Type   = PropertyType::Number;
                p.Number = value.as<double>();
            }
            else if ( value.is<std::string>() )
            {
                p.Type = PropertyType::String;
                p.Str  = value.as<std::string>();
            }
            else
            {
                continue; // unsupported type (table/function/...)
            }
            out.push_back( p );
        }
        return out;
    }

    void ScriptEngine::SetFrameMouseDelta( float dx, float dy )
    {
        m_Impl->MouseDx = dx;
        m_Impl->MouseDy = dy;
    }

    void ScriptEngine::NewInputFrame()
    {
        for ( Common::KeyCode key : TrackedKeys() )
        {
            const int  id   = static_cast<int>( key );
            const bool down = Input::Keyboard::IsKeyPressed( key );
            const bool prev = m_Impl->KeyDownPrev[id];
            m_Impl->KeyEdge[id]     = down && !prev; // rising edge
            m_Impl->KeyDownPrev[id] = down;
        }
    }

    std::optional<bool> ScriptEngine::ConsumeCursorLockRequest()
    {
        std::optional<bool> req = m_Impl->CursorLockRequest;
        m_Impl->CursorLockRequest.reset();
        return req;
    }
} // namespace Desert::Scripting

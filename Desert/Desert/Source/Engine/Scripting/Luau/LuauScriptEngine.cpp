#include <Engine/Scripting/Luau/LuauHost.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

namespace Desert::Scripting
{
    namespace
    {
        /// The registry key of the host pointer every native reaches through Impl::Of.
        constexpr const char* kHost = "desert.host";

        /// The script's text: a loose file on disk, else the packed archive's copy (shipped game).
        Common::ResultStr<std::string> ReadScript( const std::string& path )
        {
            if ( std::filesystem::exists( path ) )
            {
                std::ifstream file( path, std::ios::binary );
                if ( !file )
                    return Common::MakeError<std::string>( std::format( "script not readable: {}", path ) );
                return Common::MakeSuccess(
                     std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() ) );
            }
            if ( Common::Utils::FileSystem::Exists( path ) )
            {
                if ( auto packed = Common::Utils::FileSystem::ReadFileContent( path ); packed )
                    return Common::MakeSuccess( packed.ExtractValue() );
            }
            return Common::MakeError<std::string>( std::format( "script not found: {}", path ) );
        }
    } // namespace

    ScriptEngine::Impl& ScriptEngine::Impl::Of( lua_State* L )
    {
        lua_getfield( L, LUA_REGISTRYINDEX, kHost );
        auto* host = static_cast<Impl*>( lua_tolightuserdata( L, -1 ) );
        lua_pop( L, 1 );
        return *host;
    }

    ScriptEngine::ScriptEngine( Core::Scene* scene, Assets::AssetManager* assetManager )
         : m_Impl( std::make_unique<Impl>() )
    {
        m_Impl->Scene   = scene;
        m_Impl->Assets  = assetManager;
        m_Impl->Context = Core::WorldContext{ scene, assetManager, &m_Impl->Timers };

        Impl* host      = m_Impl.get();
        m_Impl->Runtime = std::make_unique<LuauRuntime>( LuauLimits{},
                                                         [host]( lua_State* L )
                                                         {
                                                             lua_pushlightuserdata( L, host );
                                                             lua_setfield( L, LUA_REGISTRYINDEX, kHost );
                                                             lua_newtable( L );
                                                             host->WorldVars = lua_ref( L, -1 );
                                                             lua_pop( L, 1 );

                                                             RegisterHostNatives( L );
                                                         } );
    }

    ScriptEngine::~ScriptEngine() = default;

    Common::BoolResultStr ScriptEngine::RunString( const std::string& code )
    {
        const Core::WorldContext::Scope world( m_Impl->Context );
        std::string                     output;
        Common::BoolResultStr           ran = m_Impl->Runtime->Eval( code, output );
        m_Impl->Settle();
        return ran;
    }

    Common::BoolResultStr ScriptEngine::EvalToString( const std::string& code, std::string& output )
    {
        output.clear();
        const Core::WorldContext::Scope world( m_Impl->Context );
        Common::BoolResultStr           ran = m_Impl->Runtime->Eval( code, output );
        m_Impl->Settle();
        return ran;
    }

    Common::BoolResultStr ScriptEngine::LoadEntityScript( uint32_t entity, uint32_t slot, const std::string& path )
    {
        Common::ResultStr<std::string> source = ReadScript( path );
        if ( !source.IsSuccess() )
            return Common::MakeError<bool>( source.GetError() );

        Impl&          impl = *m_Impl;
        const uint64_t key  = Impl::SlotKey( entity, slot );
        // The replaced sandbox's callables (its pending timers) are stale whether or not the new code runs.
        ++impl.Generations[key];
        impl.LastUpdateError.erase( key ); // fresh sandbox -> fresh error state

        entt::registry*    registry = impl.Scene != nullptr ? &impl.Scene->GetRegistry() : nullptr;
        const entt::entity handle   = static_cast<entt::entity>( entity );
        impl.CurrentOwner           = key; // Timer.after at the top level belongs to this slot
        const Core::WorldContext::Scope world( impl.Context );
        Common::ResultStr<LuauSlot>     loaded = impl.Runtime->Load(
             path, source.GetValue(),
             { EntityBinding( "self", [registry, handle]() { return LuauEntityRef{ registry, handle }; } ) } );
        impl.Settle();
        if ( !loaded.IsSuccess() )
            return Common::MakeError<bool>( loaded.GetError() );

        std::vector<LuauSlot>& slots = impl.Slots[entity];
        if ( slot >= slots.size() )
            slots.resize( slot + 1, 0 );
        if ( slots[slot] != 0 )
            impl.Runtime->Release( slots[slot] );
        slots[slot] = loaded.GetValue();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScriptEngine::Impl::CallSlot( uint32_t entity, uint32_t slot, const char* function,
                                                        std::span<const Reflection::Value> args )
    {
        const LuauSlot target = SlotOf( entity, slot );
        if ( target == 0 || !Runtime->Defines( target, function ) )
            return BOOLSUCCESS;
        CurrentOwner = SlotKey( entity, slot ); // Timer.after ownership
        const Core::WorldContext::Scope world( Context );
        Common::BoolResultStr           called = Runtime->Call( target, function, args );
        Settle();
        return called;
    }

    void ScriptEngine::Impl::Settle()
    {
        // entity:destroy() inside a script: its slots are released here, once no script code runs.
        std::vector<uint32_t> released;
        released.swap( PendingRelease );
        for ( uint32_t entity : released )
            ReleaseEntity( entity );
    }

    void ScriptEngine::Impl::ReleaseEntity( uint32_t entity )
    {
        if ( auto it = Slots.find( entity ); it != Slots.end() )
        {
            for ( LuauSlot slot : it->second )
                if ( slot != 0 )
                    Runtime->Release( slot );
            Slots.erase( it );
        }
    }

    void ScriptEngine::CallStart( uint32_t entity, uint32_t slot )
    {
        if ( Common::BoolResultStr r = m_Impl->CallSlot( entity, slot, "OnStart", {} ); !r.IsSuccess() )
            LOG_ERROR( "[Lua] OnStart error: {}", r.GetError() );
    }

    void ScriptEngine::CallAnimationNotify( uint32_t entity, uint32_t slot, const char* callback,
                                            const std::string& name )
    {
        const Reflection::Value arg = Reflection::Value::String( name );
        if ( Common::BoolResultStr r = m_Impl->CallSlot( entity, slot, callback, { &arg, 1 } ); !r.IsSuccess() )
            LOG_ERROR( "[Lua] {} error: {}", callback, r.GetError() );
    }

    void ScriptEngine::BroadcastUIMessage( const std::string& message )
    {
        const Reflection::Value arg = Reflection::Value::String( message );
        // A copy: an answering script may destroy entities (their slots are released after it returns).
        std::vector<std::pair<uint32_t, uint32_t>> targets;
        for ( const auto& [entity, slots] : m_Impl->Slots )
            for ( uint32_t slot = 0; slot < static_cast<uint32_t>( slots.size() ); ++slot )
                targets.emplace_back( entity, slot );
        for ( const auto& [entity, slot] : targets )
            if ( Common::BoolResultStr r = m_Impl->CallSlot( entity, slot, "OnUIMessage", { &arg, 1 } );
                 !r.IsSuccess() )
                LOG_ERROR( "[Lua] OnUIMessage error: {}", r.GetError() );
    }

    void ScriptEngine::CallUpdate( uint32_t entity, uint32_t slot, float dt )
    {
        const Reflection::Value arg = Reflection::Value::Float( dt );
        const uint64_t          key = Impl::SlotKey( entity, slot );
        Common::BoolResultStr   r   = m_Impl->CallSlot( entity, slot, "OnUpdate", { &arg, 1 } );
        if ( r.IsSuccess() )
        {
            m_Impl->LastUpdateError.erase( key );
            return;
        }
        if ( m_Impl->LastUpdateError[key] != r.GetError() )
        {
            m_Impl->LastUpdateError[key] = r.GetError();
            LOG_ERROR( "[Lua] OnUpdate error: {}", r.GetError() );
        }
    }

    void ScriptEngine::ApplyProperties( uint32_t entity, uint32_t slot, const std::vector<ScriptProperty>& props )
    {
        const LuauSlot target = m_Impl->SlotOf( entity, slot );
        if ( target == 0 )
            return;
        for ( const auto& p : props )
        {
            switch ( p.Type )
            {
                case PropertyType::Number:
                    m_Impl->Runtime->SetTableField( target, "Properties", p.Name,
                                                    Reflection::Value::Double( p.Number ) );
                    break;
                case PropertyType::Bool:
                    m_Impl->Runtime->SetTableField( target, "Properties", p.Name,
                                                    Reflection::Value::Bool( p.Bool ) );
                    break;
                case PropertyType::String:
                    m_Impl->Runtime->SetTableField( target, "Properties", p.Name,
                                                    Reflection::Value::String( p.Str ) );
                    break;
            }
        }
    }

    void ScriptEngine::Release( uint32_t entity )
    {
        m_Impl->ReleaseEntity( entity );
    }

    void ScriptEngine::TrimSlots( uint32_t entity, uint32_t count )
    {
        auto it = m_Impl->Slots.find( entity );
        if ( it != m_Impl->Slots.end() && it->second.size() > count )
        {
            for ( std::size_t i = count; i < it->second.size(); ++i )
                if ( it->second[i] != 0 )
                    m_Impl->Runtime->Release( it->second[i] );
            it->second.resize( count );
        }
    }

    void ScriptEngine::TickTimers( float dt )
    {
        // A callable of a released or reloaded slot is dead (Impl::Generations) and is dropped unfired.
        m_Impl->Timers.Tick( dt );
    }

    std::vector<ScriptProperty> ReadScriptProperties( const std::string& path )
    {
        std::vector<ScriptProperty>    out;
        Common::ResultStr<std::string> source = ReadScript( path );
        if ( !source.IsSuccess() )
            return out;

        // A throwaway runtime with no engine modules: the top level runs, `Properties` is read back.
        LuauRuntime                 runtime;
        Common::ResultStr<LuauSlot> slot = runtime.Load( path, source.GetValue(), {} );
        if ( !slot.IsSuccess() )
            return out;

        for ( const LuauTableEntry& entry : runtime.ReadTable( slot.GetValue(), "Properties" ) )
        {
            ScriptProperty p;
            p.Name = entry.Key;
            if ( const bool* b = entry.Value.Get<bool>() )
            {
                p.Type = PropertyType::Bool;
                p.Bool = *b;
            }
            else if ( const double* d = entry.Value.Get<double>() )
            {
                p.Type   = PropertyType::Number;
                p.Number = *d;
            }
            else if ( const std::string* str = entry.Value.Get<std::string>() )
            {
                p.Type = PropertyType::String;
                p.Str  = *str;
            }
            else
                continue;
            out.push_back( p );
        }
        return out;
    }

} // namespace Desert::Scripting

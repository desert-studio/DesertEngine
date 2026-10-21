#include <Engine/Scripting/Luau/LuauHost.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Libraries/InputLibrary.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

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

        /// Whether a held value fits the declaration it is applied to: the same kind (and item kind).
        bool Fits( const ScriptProperty& value, const ScriptPropertyDecl& decl )
        {
            return value.Type == decl.Default.Type &&
                   ( value.Type != PropertyType::Array || value.ElementType == decl.ElementType );
        }

        /// The comment block a script opens with (`--` lines before the first line of code, `--!` directives and
        /// blank lines skipped), its markers stripped, one line per line.
        std::string LeadingComment( const std::string& source )
        {
            std::string text;
            std::size_t at = 0;
            while ( at < source.size() )
            {
                std::size_t end = source.find( '\n', at );
                if ( end == std::string::npos )
                    end = source.size();
                std::string_view line( source.data() + at, end - at );
                at = end + 1;
                while ( !line.empty() && ( line.front() == ' ' || line.front() == '\t' ) )
                    line.remove_prefix( 1 );
                while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) )
                    line.remove_suffix( 1 );
                if ( line.empty() )
                {
                    if ( !text.empty() )
                        break; // a blank line closes the block
                    continue;
                }
                if ( !line.starts_with( "--" ) )
                    break;
                if ( line.starts_with( "--!" ) )
                    continue;
                line.remove_prefix( 2 );
                if ( !line.empty() && line.front() == ' ' )
                    line.remove_prefix( 1 );
                if ( !text.empty() )
                    text += '\n';
                text += line;
            }
            return text;
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
        m_Impl->Context = Core::WorldContext{ scene, assetManager, &m_Impl->Timers, &m_Impl->PlayerInput, this };

        Impl* host      = m_Impl.get();
        m_Impl->Runtime = std::make_unique<LuauRuntime>(
             LuauLimits{},
             [host]( lua_State* L )
             {
                 lua_pushlightuserdata( L, host );
                 lua_setfield( L, LUA_REGISTRYINDEX, kHost );
                 lua_newtable( L );
                 host->WorldVars = lua_ref( L, -1 );
                 lua_pop( L, 1 );

                 RegisterHostNatives( L );
             },
             LuauDebugging::Attachable );
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
        Common::ResultStr<std::string> const source = ReadScript( path );
        if ( !source.IsSuccess() )
            return Common::MakeError<bool>( source.GetError() );

        Impl&          impl = *m_Impl;
        const uint64_t key  = Impl::SlotKey( entity, slot );
        // The replaced sandbox's callables (its pending timers) are stale whether or not the new code runs.
        ++impl.Generations[key];
        impl.LastUpdateError.erase( key ); // fresh sandbox -> fresh error state
        impl.StaleProperties.erase( key );
        impl.SyncedOverrides.erase( key ); // the new sandbox starts from its declared defaults

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
        for ( uint32_t const entity : released )
            ReleaseEntity( entity );
    }

    void ScriptEngine::Impl::ReleaseEntity( uint32_t entity )
    {
        if ( auto it = Slots.find( entity ); it != Slots.end() )
        {
            for ( LuauSlot const slot : it->second )
                if ( slot != 0 )
                    Runtime->Release( slot );
            Slots.erase( it );
        }
    }

    void ScriptEngine::CallStart( uint32_t entity, uint32_t slot )
    {
        if ( Common::BoolResultStr const r = m_Impl->CallSlot( entity, slot, "OnStart", {} ); !r.IsSuccess() )
            LOG_ERROR( "[Lua] OnStart error: {}", r.GetError() );
    }

    void ScriptEngine::CallSlotFunction( uint32_t entity, uint32_t slot, const char* function,
                                         const std::string& argument )
    {
        const Reflection::Value arg = Reflection::Value::String( argument );
        if ( Common::BoolResultStr r = m_Impl->CallSlot( entity, slot, function, { &arg, 1 } ); !r.IsSuccess() )
            LOG_ERROR( "[Lua] {} error: {}", function, r.GetError() );
    }

    void ScriptEngine::CallSlotFunctionWith( uint32_t entity, uint32_t slot, const char* function,
                                             std::span<const Reflection::Value> arguments )
    {
        if ( Common::BoolResultStr r = m_Impl->CallSlot( entity, slot, function, arguments ); !r.IsSuccess() )
            LOG_ERROR( "[Lua] {} error: {}", function, r.GetError() );
    }

    void ScriptEngine::CallOverlap( uint32_t entity, uint32_t slot, const char* callback, entt::registry& registry,
                                    entt::entity other )
    {
        const LuauSlot target = m_Impl->SlotOf( entity, slot );
        if ( target == 0 || !m_Impl->Runtime->Defines( target, callback ) )
            return;
        m_Impl->CurrentOwner = Impl::SlotKey( entity, slot ); // Timer.after ownership
        const Core::WorldContext::Scope world( m_Impl->Context );
        if ( Common::BoolResultStr r = m_Impl->Runtime->CallWithEntity( target, callback, registry, other );
             !r.IsSuccess() )
            LOG_ERROR( "[Lua] {} error: {}", callback, r.GetError() );
        m_Impl->Settle();
    }

    void ScriptEngine::CallHit( uint32_t entity, uint32_t slot, entt::registry& registry, entt::entity other,
                                const glm::vec3& point, const glm::vec3& normal, float impulse )
    {
        const LuauSlot target = m_Impl->SlotOf( entity, slot );
        if ( target == 0 || !m_Impl->Runtime->Defines( target, "OnHit" ) )
            return;
        m_Impl->CurrentOwner = Impl::SlotKey( entity, slot ); // Timer.after ownership
        const Core::WorldContext::Scope        world( m_Impl->Context );
        const std::array<Reflection::Value, 3> after = {
             Reflection::Value::Vec3( { point.x, point.y, point.z } ),
             Reflection::Value::Vec3( { normal.x, normal.y, normal.z } ), Reflection::Value::Float( impulse ) };
        if ( Common::BoolResultStr r = m_Impl->Runtime->CallWithEntity( target, "OnHit", registry, other, after );
             !r.IsSuccess() )
            LOG_ERROR( "[Lua] OnHit error: {}", r.GetError() );
        m_Impl->Settle();
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
            if ( Common::BoolResultStr const r = m_Impl->CallSlot( entity, slot, "OnUIMessage", { &arg, 1 } );
                 !r.IsSuccess() )
                LOG_ERROR( "[Lua] OnUIMessage error: {}", r.GetError() );
    }

    void ScriptEngine::CallUpdate( uint32_t entity, uint32_t slot, float dt )
    {
        const Reflection::Value     arg = Reflection::Value::Float( dt );
        const uint64_t              key = Impl::SlotKey( entity, slot );
        Common::BoolResultStr const r   = m_Impl->CallSlot( entity, slot, "OnUpdate", { &arg, 1 } );
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
        const std::vector<ScriptPropertyDecl>* declared = m_Impl->Runtime->Declarations( target );
        if ( declared == nullptr )
            return;
        Core::Scene* const      scene = m_Impl->Scene;
        const LuauPropertyWorld world{ .Registry   = scene != nullptr ? &scene->GetRegistry() : nullptr,
                                       .FindEntity = [scene]( std::uint64_t uuid ) -> entt::entity
                                       {
                                           if ( scene == nullptr )
                                               return entt::null;
                                           const auto found = scene->FindEntityByID( Common::UUID( uuid ) );
                                           return found ? static_cast<entt::entity>( found->get().GetHandle() )
                                                        : entt::entity( entt::null );
                                       } };
        const uint64_t          key = Impl::SlotKey( entity, slot );
        for ( const ScriptProperty& p : props )
        {
            const auto decl = std::find_if( declared->begin(), declared->end(),
                                            [&]( const ScriptPropertyDecl& d ) { return d.Name == p.Name; } );
            // A value the script no longer declares, or declares as another kind (the script changed under the
            // scene): not applied, said once per slot and name — Details shows the same row as stale.
            if ( decl == declared->end() || !Fits( p, *decl ) )
            {
                if ( m_Impl->StaleProperties[key].insert( p.Name ).second )
                    LOG_ERROR( "[Script] property '{}' (entity {}, slot {}) is not applied: the script {}", p.Name,
                               entity, slot,
                               decl == declared->end() ? "no longer declares it"
                                                       : std::format( "declares it as {} now",
                                                                      PropertyTypeName( decl->Default.Type ) ) );
                continue;
            }
            m_Impl->Runtime->SetProperty( target, p, world );
        }
    }

    void ScriptEngine::SyncProperties( uint32_t entity, uint32_t slot,
                                       const std::vector<ScriptProperty>& overrides )
    {
        ApplyProperties( entity, slot, overrides );
        const LuauSlot target = m_Impl->SlotOf( entity, slot );
        if ( target == 0 )
            return;
        const std::vector<ScriptPropertyDecl>* declared = m_Impl->Runtime->Declarations( target );
        if ( declared == nullptr )
            return;

        std::unordered_set<std::string> held;
        for ( const ScriptProperty& p : overrides )
            held.insert( p.Name );
        std::unordered_set<std::string>& synced = m_Impl->SyncedOverrides[Impl::SlotKey( entity, slot )];
        bool                             gone   = false;
        for ( const std::string& name : synced )
            gone = gone || !held.contains( name );
        if ( gone )
        {
            Core::Scene* const      scene = m_Impl->Scene;
            const LuauPropertyWorld world{ .Registry   = scene != nullptr ? &scene->GetRegistry() : nullptr,
                                           .FindEntity = [scene]( std::uint64_t uuid ) -> entt::entity
                                           {
                                               if ( scene == nullptr )
                                                   return entt::null;
                                               const auto found = scene->FindEntityByID( Common::UUID( uuid ) );
                                               return found ? static_cast<entt::entity>( found->get().GetHandle() )
                                                            : entt::entity( entt::null );
                                           } };
            for ( const ScriptPropertyDecl& decl : *declared )
                if ( synced.contains( decl.Name ) && !held.contains( decl.Name ) )
                    m_Impl->Runtime->SetProperty( target, decl.Default, world );
        }
        synced = std::move( held );
    }

    void ScriptEngine::ReadBackProperties( uint32_t entity, uint32_t slot, const std::vector<std::string>& names,
                                           std::vector<ScriptProperty>& props )
    {
        const LuauSlot source = m_Impl->SlotOf( entity, slot );
        if ( source == 0 || names.empty() )
            return;
        const std::vector<ScriptPropertyDecl>* declared = m_Impl->Runtime->Declarations( source );
        if ( declared == nullptr )
            return;
        for ( const ScriptPropertyDecl& decl : *declared )
        {
            if ( std::find( names.begin(), names.end(), decl.Name ) == names.end() )
                continue;
            ScriptProperty read;
            if ( !m_Impl->Runtime->ReadProperty( source, decl.Name, decl.Default.Type, read ) )
                continue;
            read.Name       = decl.Name;
            const auto held = std::find_if( props.begin(), props.end(),
                                            [&]( const ScriptProperty& p ) { return p.Name == decl.Name; } );
            if ( held == props.end() )
                props.push_back( std::move( read ) );
            else
                *held = std::move( read );
        }
    }

    void ScriptEngine::ApplySaveGameProperties()
    {
        if ( m_Impl->Scene == nullptr )
            return;
        entt::registry& registry = m_Impl->Scene->GetRegistry();
        for ( const auto& [entityId, slots] : m_Impl->Slots )
        {
            const auto entity = static_cast<entt::entity>( entityId );
            if ( !registry.valid( entity ) || !registry.has<ECS::ScriptComponent>( entity ) )
                continue;
            const auto& scripts = registry.get<ECS::ScriptComponent>( entity ).Scripts;
            for ( std::size_t i = 0; i < slots.size() && i < scripts.size(); ++i )
            {
                if ( slots[i] == 0 )
                    continue;
                std::vector<ScriptProperty> saved;
                for ( const std::string& name : scripts[i].SaveGameProperties )
                    for ( const ScriptProperty& p : scripts[i].Properties )
                        if ( p.Name == name )
                            saved.push_back( p );
                ApplyProperties( entityId, static_cast<uint32_t>( i ), saved );
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

    Common::ResultStr<std::vector<std::string>> ReadScriptSaveGameProperties( const std::string& path )
    {
        using Names                           = std::vector<std::string>;
        Common::ResultStr<std::string> source = ReadScript( path );
        if ( !source.IsSuccess() )
            return Common::MakeError<Names>( source.GetError() );

        // A throwaway runtime with no engine modules: the top level runs, the two tables are read back.
        LuauRuntime                 runtime;
        Common::ResultStr<LuauSlot> slot = runtime.Load( path, source.GetValue(), {} );
        if ( !slot.IsSuccess() )
            return Common::MakeFormattedError<Names>( "script '{}' top level failed: {}", path, slot.GetError() );

        Common::ResultStr<Names> declared = runtime.ReadStringList( slot.GetValue(), "SaveGameProperties" );
        if ( !declared.IsSuccess() )
            return Common::MakeFormattedError<Names>( "script '{}': {}", path, declared.GetError() );
        Names                                  list       = declared.ExtractValue();
        const std::vector<ScriptPropertyDecl>& properties = *runtime.Declarations( slot.GetValue() );
        for ( const std::string& name : list )
        {
            const auto decl  = std::find_if( properties.begin(), properties.end(),
                                             [&]( const ScriptPropertyDecl& d ) { return d.Name == name; } );
            const bool known = decl != properties.end();
            if ( known &&
                 !( decl->Default.Type == PropertyType::Number || decl->Default.Type == PropertyType::Int ||
                    decl->Default.Type == PropertyType::Bool || decl->Default.Type == PropertyType::String ||
                    decl->Default.Type == PropertyType::Enum ) )
                return Common::MakeFormattedError<Names>( "script '{}': SaveGameProperties names '{}', a {}; a "
                                                          "save game holds numbers, booleans, strings "
                                                          "and enumerators",
                                                          path, name, PropertyTypeName( decl->Default.Type ) );
            if ( !known )
                return Common::MakeFormattedError<Names>(
                     "script '{}': SaveGameProperties names '{}', which its Properties table does not declare",
                     path, name );
        }
        return Common::MakeSuccess( std::move( list ) );
    }

    ScriptSchema ReadScriptSchema( const std::string& path )
    {
        ScriptSchema schema;
        const auto   fail = [&]( ScriptStatus status, std::string error )
        {
            schema.Status    = status;
            schema.ErrorLine = ScriptErrorLine( error );
            schema.Error     = std::move( error );
            return schema;
        };
        if ( !std::filesystem::exists( path ) && !Common::Utils::FileSystem::Exists( path ) )
            return fail( ScriptStatus::NotFound, std::format( "script not found: {}", path ) );
        Common::ResultStr<std::string> source = ReadScript( path );
        if ( !source.IsSuccess() )
            return fail( ScriptStatus::Error, source.GetError() );
        schema.Description = LeadingComment( source.GetValue() );

        // A throwaway runtime with no engine modules: the top level runs, the declarations are read back.
        LuauRuntime                       runtime;
        Common::ResultStr<LuauSlot> const slot = runtime.Load( path, source.GetValue(), {} );
        if ( !slot.IsSuccess() )
            return fail( ScriptStatus::Error, slot.GetError() );
        schema.Properties = *runtime.Declarations( slot.GetValue() );

        Common::ResultStr<std::vector<std::string>> buttons =
             runtime.ReadStringList( slot.GetValue(), "CallInEditor" );
        if ( !buttons.IsSuccess() )
            return fail( ScriptStatus::Error, std::format( "{}: {}", path, buttons.GetError() ) );
        for ( const std::string& name : buttons.GetValue() )
        {
            if ( !runtime.Defines( slot.GetValue(), name.c_str() ) )
                return fail(
                     ScriptStatus::Error,
                     std::format( "{}: CallInEditor names '{}', which the script does not define as a function",
                                  path, name ) );
            schema.EditorFunctions.push_back( { name } );
        }
        return schema;
    }

    int ScriptErrorLine( const std::string& error )
    {
        // Luau prefixes a located error with `<chunk>:<line>:`; a chunk is a path, which may itself hold a colon
        // (`C:/...`), so the first colon FOLLOWED BY DIGITS AND A COLON is the one.
        for ( std::size_t colon = error.find( ':' ); colon != std::string::npos;
              colon             = error.find( ':', colon + 1 ) )
        {
            std::size_t end = colon + 1;
            while ( end < error.size() && error[end] >= '0' && error[end] <= '9' )
                ++end;
            if ( end > colon + 1 && end < error.size() && error[end] == ':' )
                return std::stoi( error.substr( colon + 1, end - colon - 1 ) );
        }
        return 0;
    }

    Common::BoolResultStr ScriptEngine::CallFunction( uint32_t entity, uint32_t slot, const std::string& function )
    {
        const LuauSlot target = m_Impl->SlotOf( entity, slot );
        if ( target == 0 )
            return Common::MakeFormattedError<bool>( "no script runs in slot {} of entity {}", slot, entity );
        if ( !m_Impl->Runtime->Defines( target, function.c_str() ) )
            return Common::MakeFormattedError<bool>( "the script defines no function '{}'", function );
        return m_Impl->CallSlot( entity, slot, function.c_str(), {} );
    }

    const std::string* ScriptEngine::SlotError( uint32_t entity, uint32_t slot ) const
    {
        const auto found = m_Impl->LastUpdateError.find( Impl::SlotKey( entity, slot ) );
        return found == m_Impl->LastUpdateError.end() ? nullptr : &found->second;
    }

    void ScriptEngine::TickPlayerInput( entt::registry& registry, const float deltaSeconds )
    {
        if ( !m_Impl->PlayerInputBegun )
        {
            m_Impl->PlayerInputBegun = true;
            if ( m_Impl->Assets != nullptr )
                m_Impl->PlayerInput.BeginPlay( registry, *m_Impl->Assets,
                                               m_Impl->Scene != nullptr ? m_Impl->Scene->GetPlayerPawn()
                                                                        : entt::entity( entt::null ) );
            else if ( !registry.view<ECS::EnhancedInputPlayerComponent>().empty() )
                LOG_ERROR( "[Input] this world plays without an asset manager: the player's mapping contexts "
                           "cannot be read" );
        }
        m_Impl->PlayerInput.Tick( Libraries::InputLibrary::MouseDelta(), deltaSeconds );
    }

    void ScriptEngine::DeliverGameModeEvents( entt::registry& registry )
    {
        if ( m_Impl->Scene == nullptr )
            return;
        for ( const Core::GameModeEvent& event : m_Impl->Scene->GetGameMode().TakeEvents() )
        {
            const bool died = event.Kind == Core::GameModeEventKind::PawnDied;
            if ( died )
                m_Impl->PlayerInput.UnpossessPawn();
            else if ( m_Impl->Assets != nullptr )
                m_Impl->PlayerInput.PossessPawn( registry, *m_Impl->Assets, event.Pawn );
            const char* hook = died ? "OnPawnDied" : "OnPlayerRestarted";
            // A copy: an answering script may destroy entities (their slots are released after it returns).
            std::vector<std::pair<uint32_t, uint32_t>> targets;
            for ( const auto& [entity, slots] : m_Impl->Slots )
                for ( uint32_t slot = 0; slot < static_cast<uint32_t>( slots.size() ); ++slot )
                    targets.emplace_back( entity, slot );
            for ( const auto& [entity, slot] : targets )
            {
                const LuauSlot target = m_Impl->SlotOf( entity, slot );
                if ( target == 0 || !m_Impl->Runtime->Defines( target, hook ) )
                    continue;
                m_Impl->CurrentOwner = Impl::SlotKey( entity, slot );
                const Core::WorldContext::Scope world( m_Impl->Context );
                if ( Common::BoolResultStr r =
                          m_Impl->Runtime->CallWithEntity( target, hook, registry, event.Pawn );
                     !r.IsSuccess() )
                    LOG_ERROR( "[Lua] {} error: {}", hook, r.GetError() );
                m_Impl->Settle();
            }
        }
    }

    void ScriptEngine::EndPlayerInput()
    {
        if ( !m_Impl->PlayerInputBegun )
            return;
        m_Impl->PlayerInput.EndPlay();
        m_Impl->PlayerInputBegun = false;
    }

    Input::LocalPlayerInput& ScriptEngine::PlayerInput()
    {
        return m_Impl->PlayerInput;
    }
} // namespace Desert::Scripting

#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/SaveGame.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <cstdint>
#include <string>

namespace Desert::Scripting
{
    // savegame.save / load / exists / delete / list / scene — UGameplayStatics::SaveGameToSlot, LoadGameFromSlot,
    // DoesSaveGameExist, DeleteGameInSlot, the platform save list, over Core/SaveGame.hpp (the same calls C++
    // gameplay makes). `user` is the UserIndex and defaults to 0. A failure returns false (nil for a query) + the
    // reason, never raises into the script; a load that applied with skips returns true + the list of what it
    // skipped (a missing entity, a field whose type changed), each line naming the entity and field.
    namespace
    {
        std::uint32_t UserArg( lua_State* L, int index )
        {
            return static_cast<std::uint32_t>( luaL_optinteger( L, index, 0 ) );
        }

        int Fail( lua_State* L, const std::string& reason, bool asNil = false )
        {
            if ( asNil )
                lua_pushnil( L );
            else
                lua_pushboolean( L, 0 );
            lua_pushstring( L, reason.c_str() );
            return 2;
        }

        void PushStringList( lua_State* L, const std::vector<std::string>& list )
        {
            lua_createtable( L, static_cast<int>( list.size() ), 0 );
            for ( std::size_t i = 0; i < list.size(); ++i )
            {
                lua_pushstring( L, list[i].c_str() );
                lua_rawseti( L, -2, static_cast<int>( i + 1 ) );
            }
        }

        int Save( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            const std::string   slot = luaL_checkstring( L, 1 );
            if ( host.Scene == nullptr )
                return Fail( L, "savegame.save: no scene is running" );
            const auto saved = Core::SaveGameToSlot( *host.Scene, slot, UserArg( L, 2 ) );
            if ( !saved )
                return Fail( L, saved.GetError() );
            lua_pushboolean( L, 1 );
            return 1;
        }

        int Load( lua_State* L )
        {
            ScriptEngine::Impl& host = ScriptEngine::Impl::Of( L );
            const std::string   slot = luaL_checkstring( L, 1 );
            if ( host.Scene == nullptr )
                return Fail( L, "savegame.load: no scene is running" );
            const auto loaded = Core::LoadGameFromSlot( *host.Scene, slot, UserArg( L, 2 ) );
            if ( !loaded )
                return Fail( L, loaded.GetError() );
            // The load wrote the SaveGame properties into the script slots; the running sandboxes get them NOW,
            // so the calling script reads the loaded values on its next line and ScriptSystem's read-back after
            // this OnUpdate does not copy the pre-load values over them.
            entt::registry& registry = host.Registry();
            for ( const auto& [entityId, slots] : host.Slots )
            {
                const auto entity = static_cast<entt::entity>( entityId );
                if ( !registry.valid( entity ) || !registry.has<ECS::ScriptComponent>( entity ) )
                    continue;
                const auto& scripts = registry.get<ECS::ScriptComponent>( entity ).Scripts;
                for ( std::size_t i = 0; i < slots.size() && i < scripts.size(); ++i )
                {
                    if ( slots[i] == 0 )
                        continue;
                    for ( const std::string& name : scripts[i].SaveGameProperties )
                        for ( const ScriptProperty& p : scripts[i].Properties )
                        {
                            if ( p.Name != name )
                                continue;
                            switch ( p.Type )
                            {
                                case PropertyType::Number:
                                    host.Runtime->SetTableField( slots[i], "Properties", p.Name,
                                                                 Reflection::Value::Double( p.Number ) );
                                    break;
                                case PropertyType::Bool:
                                    host.Runtime->SetTableField( slots[i], "Properties", p.Name,
                                                                 Reflection::Value::Bool( p.Bool ) );
                                    break;
                                case PropertyType::String:
                                    host.Runtime->SetTableField( slots[i], "Properties", p.Name,
                                                                 Reflection::Value::String( p.Str ) );
                                    break;
                            }
                        }
                }
            }
            lua_pushboolean( L, 1 );
            PushStringList( L, loaded.GetValue().Problems );
            return 2;
        }

        int Exists( lua_State* L )
        {
            const std::string slot   = luaL_checkstring( L, 1 );
            const auto        exists = Core::DoesSaveGameExist( slot, UserArg( L, 2 ) );
            if ( !exists )
                return Fail( L, exists.GetError() );
            lua_pushboolean( L, exists.GetValue() ? 1 : 0 );
            return 1;
        }

        int Delete( lua_State* L )
        {
            const std::string slot    = luaL_checkstring( L, 1 );
            const auto        deleted = Core::DeleteGameInSlot( slot, UserArg( L, 2 ) );
            if ( !deleted )
                return Fail( L, deleted.GetError() );
            lua_pushboolean( L, 1 );
            return 1;
        }

        int List( lua_State* L )
        {
            const auto slots = Core::ListSaveGameSlots( UserArg( L, 1 ) );
            if ( !slots )
                return Fail( L, slots.GetError(), true );
            PushStringList( L, slots.GetValue() );
            return 1;
        }

        // The scene a slot was saved in ({guid=, name=}), so a script can level.open it before savegame.load.
        int SceneOf( lua_State* L )
        {
            const std::string slot = luaL_checkstring( L, 1 );
            const auto        root = Core::SaveGameRoot();
            if ( !root )
                return Fail( L, root.GetError(), true );
            const auto document = Core::ReadSaveGameSlot( root.GetValue(), slot, UserArg( L, 2 ) );
            if ( !document )
                return Fail( L, document.GetError(), true );
            const auto scene = Core::SaveGameSceneOf( document.GetValue() );
            if ( !scene )
                return Fail( L, scene.GetError(), true );
            lua_createtable( L, 0, 2 );
            lua_pushstring( L, scene.GetValue().Guid.c_str() );
            lua_setfield( L, -2, "guid" );
            lua_pushstring( L, scene.GetValue().Name.c_str() );
            lua_setfield( L, -2, "name" );
            return 1;
        }
    } // namespace

    void RegisterSaveGameBindings( lua_State* L )
    {
        constexpr luaL_Reg kSaveGame[] = { { "save", &Save },     { "load", &Load }, { "exists", &Exists },
                                           { "delete", &Delete }, { "list", &List }, { "scene", &SceneOf },
                                           { nullptr, nullptr } };
        luaL_register( L, "savegame", kSaveGame );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting

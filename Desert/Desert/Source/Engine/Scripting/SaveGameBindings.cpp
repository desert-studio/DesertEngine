#include "Internal/ScriptRuntime.hpp"

#include <Engine/Core/SaveGame.hpp>

#include <tuple>

namespace Desert::Scripting
{
    // savegame.save / load / exists / delete / list / scene — UGameplayStatics::SaveGameToSlot, LoadGameFromSlot,
    // DoesSaveGameExist, DeleteGameInSlot, the platform save list, over Core/SaveGame.hpp (the same calls C++
    // gameplay makes). `user` is the UserIndex and defaults to 0. A failure returns false + the reason, never
    // throws into the script; a load that applied with skips returns true + the list of what it skipped (a missing
    // entity, a field whose type changed), each line naming the entity and field.
    void RegisterSaveGameBindings( ScriptEngine::Impl& implRef )
    {
        ScriptEngine::Impl* impl     = &implRef;
        sol::table          savegame = implRef.Lua.create_named_table( "savegame" );

        savegame.set_function(
             "save",
             [impl]( const std::string&           slot,
                     sol::optional<std::uint32_t> user ) -> std::tuple<bool, sol::optional<std::string>>
             {
                 if ( impl->Scene == nullptr )
                     return { false, std::string( "savegame.save: no scene is running" ) };
                 const auto saved = Core::SaveGameToSlot( *impl->Scene, slot, user.value_or( 0 ) );
                 if ( !saved )
                     return { false, saved.GetError() };
                 return { true, sol::nullopt };
             } );

        savegame.set_function(
             "load",
             [impl]( const std::string& slot, sol::optional<std::uint32_t> user,
                     sol::this_state state ) -> std::tuple<bool, sol::object>
             {
                 sol::state_view lua( state );
                 if ( impl->Scene == nullptr )
                     return { false,
                              sol::make_object( lua, std::string( "savegame.load: no scene is running" ) ) };
                 const auto loaded = Core::LoadGameFromSlot( *impl->Scene, slot, user.value_or( 0 ) );
                 if ( !loaded )
                     return { false, sol::make_object( lua, loaded.GetError() ) };
                 sol::table skipped = lua.create_table();
                 for ( const std::string& problem : loaded.GetValue().Problems )
                     skipped.add( problem );
                 return { true, sol::make_object( lua, skipped ) };
             } );

        savegame.set_function( "exists",
                               []( const std::string& slot, sol::optional<std::uint32_t> user )
                                    -> std::tuple<bool, sol::optional<std::string>>
                               {
                                   const auto exists = Core::DoesSaveGameExist( slot, user.value_or( 0 ) );
                                   if ( !exists )
                                       return { false, exists.GetError() };
                                   return { exists.GetValue(), sol::nullopt };
                               } );

        savegame.set_function( "delete",
                               []( const std::string& slot, sol::optional<std::uint32_t> user )
                                    -> std::tuple<bool, sol::optional<std::string>>
                               {
                                   const auto deleted = Core::DeleteGameInSlot( slot, user.value_or( 0 ) );
                                   if ( !deleted )
                                       return { false, deleted.GetError() };
                                   return { true, sol::nullopt };
                               } );

        savegame.set_function( "list",
                               []( sol::optional<std::uint32_t> user,
                                   sol::this_state state ) -> std::tuple<sol::object, sol::optional<std::string>>
                               {
                                   sol::state_view lua( state );
                                   const auto      slots = Core::ListSaveGameSlots( user.value_or( 0 ) );
                                   if ( !slots )
                                       return { sol::make_object( lua, sol::lua_nil ), slots.GetError() };
                                   sol::table names = lua.create_table();
                                   for ( const std::string& name : slots.GetValue() )
                                       names.add( name );
                                   return { sol::make_object( lua, names ), sol::nullopt };
                               } );

        // The scene a slot was saved in ({guid=, name=}), so a script can level.open it before savegame.load.
        savegame.set_function( "scene",
                               []( const std::string& slot, sol::optional<std::uint32_t> user,
                                   sol::this_state state ) -> std::tuple<sol::object, sol::optional<std::string>>
                               {
                                   sol::state_view lua( state );
                                   const auto      root = Core::SaveGameRoot();
                                   if ( !root )
                                       return { sol::make_object( lua, sol::lua_nil ), root.GetError() };
                                   const auto document =
                                        Core::ReadSaveGameSlot( root.GetValue(), slot, user.value_or( 0 ) );
                                   if ( !document )
                                       return { sol::make_object( lua, sol::lua_nil ), document.GetError() };
                                   const auto scene = Core::SaveGameSceneOf( document.GetValue() );
                                   if ( !scene )
                                       return { sol::make_object( lua, sol::lua_nil ), scene.GetError() };
                                   sol::table out = lua.create_table();
                                   out["guid"]    = scene.GetValue().Guid;
                                   out["name"]    = scene.GetValue().Name;
                                   return { sol::make_object( lua, out ), sol::nullopt };
                               } );
    }
} // namespace Desert::Scripting

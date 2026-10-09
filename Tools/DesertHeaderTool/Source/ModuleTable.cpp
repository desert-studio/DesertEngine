#include "ModuleTable.hpp"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

namespace Desert::HeaderTool
{
    namespace
    {
        struct LuaStateDeleter
        {
            void operator()( lua_State* state ) const
            {
                lua_close( state );
            }
        };
        using LuaState = std::unique_ptr<lua_State, LuaStateDeleter>;

        // The string field `key` of the table at the top of the stack; empty when it is absent.
        std::string StringField( lua_State* L, const char* key )
        {
            lua_getfield( L, -1, key );
            std::string value = lua_isstring( L, -1 ) ? lua_tostring( L, -1 ) : "";
            lua_pop( L, 1 );
            return value;
        }

        // Calls `visit` with each element of the array field `key` of the table at the top of the stack, the
        // element pushed on top of the stack.
        template <typename Visit>
        void ForEachInArray( lua_State* L, const char* key, Visit&& visit )
        {
            lua_getfield( L, -1, key );
            if ( lua_istable( L, -1 ) )
            {
                const lua_Integer count = luaL_len( L, -1 );
                for ( lua_Integer i = 1; i <= count; ++i )
                {
                    lua_geti( L, -1, i );
                    visit();
                    lua_pop( L, 1 );
                }
            }
            lua_pop( L, 1 );
        }

        std::string TopError( lua_State* L )
        {
            return lua_isstring( L, -1 ) ? lua_tostring( L, -1 ) : "(no message)";
        }
    } // namespace

    Common::ResultStr<ModuleTable> ModuleTable::Load( const std::filesystem::path& tableFile )
    {
        LuaState   owned( luaL_newstate() );
        lua_State* L = owned.get();
        luaL_openlibs( L );
        if ( luaL_dofile( L, tableFile.string().c_str() ) != LUA_OK )
            return Common::MakeError<ModuleTable>(
                 std::format( "module table {}: {}", tableFile.string(), TopError( L ) ) );
        if ( !lua_istable( L, -1 ) )
            return Common::MakeError<ModuleTable>(
                 std::format( "module table {} does not return a table", tableFile.string() ) );

        lua_getfield( L, -1, "Validate" );
        if ( lua_pcall( L, 0, 0, 0 ) != LUA_OK )
            return Common::MakeError<ModuleTable>( TopError( L ) );

        ModuleTable table;
        table.m_LegacyRoot = StringField( L, "LegacyRoot" );
        ForEachInArray( L, "Modules",
                        [&]
                        {
                            ModuleInfo module{ StringField( L, "Name" ), StringField( L, "Folder" ), {} };
                            ForEachInArray( L, "Deps",
                                            [&] { module.Deps.emplace_back( lua_tostring( L, -1 ) ); } );
                            table.m_Modules.push_back( std::move( module ) );
                        } );
        std::string badPattern;
        ForEachInArray( L, "Placement",
                        [&]
                        {
                            std::string module  = StringField( L, "Module" );
                            std::string pattern = StringField( L, "Pattern" );
                            try
                            {
                                std::regex regex( pattern, std::regex::ECMAScript );
                                table.m_Placement.push_back(
                                     { std::move( module ), std::move( pattern ), std::move( regex ) } );
                            }
                            catch ( const std::regex_error& error )
                            {
                                badPattern = std::format( "placement row of '{}': {} ({})", module, error.what(),
                                                          pattern );
                            }
                        } );
        if ( !badPattern.empty() )
            return Common::MakeError<ModuleTable>(
                 std::format( "module table {}: {}", tableFile.string(), badPattern ) );
        if ( table.m_Modules.empty() || table.m_Placement.empty() || table.m_LegacyRoot.empty() )
            return Common::MakeError<ModuleTable>(
                 std::format( "module table {} lacks Modules, Placement or LegacyRoot", tableFile.string() ) );
        return Common::MakeSuccess( std::move( table ) );
    }

    const ModuleInfo* ModuleTable::Find( std::string_view name ) const
    {
        const auto it = std::find_if( m_Modules.begin(), m_Modules.end(),
                                      [&]( const ModuleInfo& module ) { return module.Name == name; } );
        return it == m_Modules.end() ? nullptr : &*it;
    }

    std::set<std::string> ModuleTable::Closure( std::string_view name ) const
    {
        std::set<std::string>    seen;
        std::vector<std::string> pending{ std::string( name ) };
        while ( !pending.empty() )
        {
            const ModuleInfo* module = Find( pending.back() );
            pending.pop_back();
            if ( module == nullptr )
                continue;
            for ( const std::string& dep : module->Deps )
                if ( seen.insert( dep ).second )
                    pending.push_back( dep );
        }
        return seen;
    }

    std::string ModuleTable::ModuleOf( std::string_view repoRelative ) const
    {
        const auto under = [&]( std::string_view root )
        {
            return repoRelative.size() > root.size() && repoRelative.starts_with( root ) &&
                   repoRelative[root.size()] == '/';
        };
        for ( const ModuleInfo& module : m_Modules )
            if ( under( module.Folder ) )
                return module.Name;
        if ( under( m_LegacyRoot ) )
            return ModuleOfLegacy( repoRelative.substr( m_LegacyRoot.size() + 1 ) );
        return {};
    }

    std::string ModuleTable::ModuleOfLegacy( std::string_view legacyRelative ) const
    {
        const std::string path( legacyRelative );
        for ( const ModulePlacement& row : m_Placement )
            if ( std::regex_search( path, row.Regex ) )
                return row.Module;
        return {};
    }
} // namespace Desert::HeaderTool

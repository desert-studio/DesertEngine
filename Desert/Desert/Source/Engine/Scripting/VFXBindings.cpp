#include "Internal/ScriptRuntime.hpp"

#include <Engine/VFX/VFXWorld.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Scripting
{
    // VFX table: gameplay writes the scene's data channels (VFX-10, UE UNiagaraDataChannelWriter).
    //   VFX.useChannel("Impacts")                        -- registers the channel ASSET named Impacts (its name,
    //                                                       as a script names IA_Jump: <VFX root>/Impacts.dfxch)
    //   VFX.writeChannel("Impacts", {                    -- appends entries for this frame; true when all written
    //       { Position = {x, y, z}, Direction = {x, y, z}, Strength = 2.0, Team = 1 },
    //   })
    // A field is written by its type in the channel: Position / Direction {x, y, z}, Color {r, g, b, a}, Float a
    // number, Int a whole number. Entries are consumed by the scene's next VFX tick.
    namespace
    {
        // The name is resolved by the scene's AssetManager (VFXDataChannels::Use), never by reading the file here:
        // the manager is the one place assets are loaded, so a channel the editor holds (any folder) is the one a
        // script gets, and an unknown name is refused by name.
        int UseChannel( lua_State* L )
        {
            const std::string name = luaL_checkstring( L, 1 );
            auto&             host = ScriptEngine::Impl::Of( L );
            bool              used = false;
            if ( host.Scene != nullptr && host.Assets == nullptr )
            {
                LOG_ERROR( "[Lua] VFX.useChannel('{}'): no AssetManager bound to resolve the channel through",
                           name );
            }
            else if ( host.Scene != nullptr )
            {
                const auto result = host.Scene->GetVFXWorld().GetDataChannels().Use( name, *host.Assets );
                if ( !result )
                    LOG_ERROR( "[Lua] VFX.useChannel('{}'): {}", name, result.GetError() );
                used = static_cast<bool>( result );
            }
            lua_pushboolean( L, used );
            return 1;
        }

        // Component `index` (1-based) of the array table on top of the stack; 0 when absent or not a number.
        float ComponentAt( lua_State* L, int index )
        {
            lua_rawgeti( L, -1, index );
            const float v = lua_isnumber( L, -1 ) ? static_cast<float>( lua_tonumber( L, -1 ) ) : 0.0f;
            lua_pop( L, 1 );
            return v;
        }

        int WriteChannel( lua_State* L )
        {
            const std::string name = luaL_checkstring( L, 1 );
            luaL_checktype( L, 2, LUA_TTABLE );
            auto& host = ScriptEngine::Impl::Of( L );
            if ( host.Scene == nullptr )
            {
                lua_pushboolean( L, false );
                return 1;
            }
            auto&       channels = host.Scene->GetVFXWorld().GetDataChannels();
            const auto* channel  = channels.Find( name );
            if ( channel == nullptr )
            {
                LOG_ERROR( "[Lua] VFX.writeChannel: no data channel '{}' (VFX.useChannel first)", name );
                lua_pushboolean( L, false );
                return 1;
            }
            const auto        fields = channel->Layout().Fields; // the writer below appends to the same channel
            const std::size_t count  = static_cast<std::size_t>( lua_objlen( L, 2 ) );
            auto              writer = channels.Write( name, count );
            if ( !writer.IsSuccess() )
            {
                LOG_ERROR( "[Lua] VFX.writeChannel: {}", writer.GetError() );
                lua_pushboolean( L, false );
                return 1;
            }
            VFX::VFXDataChannelWriter w  = writer.ExtractValue();
            bool                      ok = true;
            for ( std::size_t i = 0; i < count; ++i )
            {
                lua_rawgeti( L, 2, static_cast<int>( i + 1 ) );
                if ( !lua_istable( L, -1 ) )
                {
                    lua_pop( L, 1 );
                    ok = false;
                    continue;
                }
                for ( const auto& f : fields )
                {
                    lua_getfield( L, -1, f.Name.c_str() );
                    if ( lua_isnil( L, -1 ) )
                    {
                        lua_pop( L, 1 );
                        continue; // an unwritten field stays zero
                    }
                    using T                       = Assets::Serialization::VFXDataChannelFieldType;
                    Common::BoolResultStr written = BOOLSUCCESS;
                    if ( f.Type == T::Float || f.Type == T::Int )
                    {
                        if ( lua_type( L, -1 ) != LUA_TNUMBER )
                            written = Common::MakeFormattedError<bool>( "field '{}' needs a number", f.Name );
                        else if ( f.Type == T::Float )
                            written = w.WriteFloat( i, f.Name, static_cast<float>( lua_tonumber( L, -1 ) ) );
                        else
                            written = w.WriteInt( i, f.Name, static_cast<std::int32_t>( lua_tointeger( L, -1 ) ) );
                    }
                    else if ( !lua_istable( L, -1 ) )
                        written = Common::MakeFormattedError<bool>( "field '{}' needs a table", f.Name );
                    else
                    {
                        const glm::vec4 c( ComponentAt( L, 1 ), ComponentAt( L, 2 ), ComponentAt( L, 3 ),
                                           ComponentAt( L, 4 ) );
                        if ( f.Type == T::Position )
                            written = w.WritePosition( i, f.Name, glm::vec3( c ) );
                        else if ( f.Type == T::Direction )
                            written = w.WriteDirection( i, f.Name, glm::vec3( c ) );
                        else
                            written = w.WriteColor( i, f.Name, c );
                    }
                    lua_pop( L, 1 );
                    if ( !written )
                    {
                        LOG_ERROR( "[Lua] VFX.writeChannel '{}' entry {}: {}", name, i + 1, written.GetError() );
                        ok = false;
                    }
                }
                lua_pop( L, 1 );
            }
            lua_pushboolean( L, ok );
            return 1;
        }
    } // namespace

    void RegisterVFXBindings( lua_State* L )
    {
        constexpr luaL_Reg kVFX[] = {
             { "useChannel", &UseChannel }, { "writeChannel", &WriteChannel }, { nullptr, nullptr } };
        luaL_register( L, "VFX", kVFX );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting

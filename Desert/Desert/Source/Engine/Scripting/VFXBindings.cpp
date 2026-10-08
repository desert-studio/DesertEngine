#include "Internal/ScriptRuntime.hpp"

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>

namespace Desert::Scripting
{
    // VFX table: gameplay writes the scene's data channels (VFX-10, UE UNiagaraDataChannelWriter).
    //   VFX.useChannel("VFX/Impacts.dfxch")             -- registers the channel (named by its file stem)
    //   VFX.writeChannel("Impacts", {                    -- appends entries for this frame; true when all written
    //       { Position = {x, y, z}, Direction = {x, y, z}, Strength = 2.0, Team = 1 },
    //   })
    // A field is written by its type in the channel: Position / Direction {x, y, z}, Color {r, g, b, a}, Float a
    // number, Int a whole number. Entries are consumed by the scene's next VFX tick.
    void RegisterVFXBindings( ScriptEngine::Impl& implRef )
    {
        auto*      impl = &implRef;
        sol::table vfx  = implRef.Lua.create_named_table( "VFX" );

        vfx["useChannel"] = [impl]( const std::string& path ) -> bool
        {
            if ( impl->Scene == nullptr )
                return false;
            const auto loaded = Assets::Serialization::LoadVFXDataChannelFile( path );
            if ( !loaded.IsSuccess() )
            {
                LOG_ERROR( "[Lua] VFX.useChannel: {}", loaded.GetError() );
                return false;
            }
            const std::string name = std::filesystem::path( path ).stem().string();
            const auto        registered =
                 impl->Scene->GetVFXWorld().GetDataChannels().Register( name, loaded.GetValue() );
            if ( !registered )
                LOG_ERROR( "[Lua] VFX.useChannel: {}", registered.GetError() );
            return static_cast<bool>( registered );
        };

        vfx["writeChannel"] = [impl]( const std::string& name, const sol::table& entries ) -> bool
        {
            if ( impl->Scene == nullptr )
                return false;
            auto&       channels = impl->Scene->GetVFXWorld().GetDataChannels();
            const auto* channel  = channels.Find( name );
            if ( channel == nullptr )
            {
                LOG_ERROR( "[Lua] VFX.writeChannel: no data channel '{}' (VFX.useChannel first)", name );
                return false;
            }
            const auto fields = channel->Layout().Fields; // the writer below appends to the same channel
            auto       writer = channels.Write( name, entries.size() );
            if ( !writer.IsSuccess() )
            {
                LOG_ERROR( "[Lua] VFX.writeChannel: {}", writer.GetError() );
                return false;
            }
            VFX::VFXDataChannelWriter& w  = writer.GetValue();
            bool                       ok = true;
            for ( std::size_t i = 0; i < entries.size(); ++i )
            {
                const sol::optional<sol::table> entry = entries[i + 1];
                if ( !entry )
                {
                    ok = false;
                    continue;
                }
                for ( const auto& f : fields )
                {
                    const sol::object value = ( *entry )[f.Name];
                    if ( !value.valid() || value.get_type() == sol::type::lua_nil )
                        continue; // an unwritten field stays zero
                    using T                       = Assets::Serialization::VFXDataChannelFieldType;
                    Common::BoolResultStr written = BOOLSUCCESS;
                    if ( f.Type == T::Float || f.Type == T::Int )
                    {
                        if ( value.get_type() != sol::type::number )
                            written = Common::MakeFormattedError<bool>( "field '{}' needs a number", f.Name );
                        else if ( f.Type == T::Float )
                            written = w.WriteFloat( i, f.Name, value.as<float>() );
                        else
                            written = w.WriteInt( i, f.Name, value.as<std::int32_t>() );
                    }
                    else if ( value.get_type() != sol::type::table )
                        written = Common::MakeFormattedError<bool>( "field '{}' needs a table", f.Name );
                    else
                    {
                        const sol::table v = value.as<sol::table>();
                        const glm::vec4  c( v.get_or( 1, 0.0f ), v.get_or( 2, 0.0f ), v.get_or( 3, 0.0f ),
                                            v.get_or( 4, 0.0f ) );
                        if ( f.Type == T::Position )
                            written = w.WritePosition( i, f.Name, glm::vec3( c ) );
                        else if ( f.Type == T::Direction )
                            written = w.WriteDirection( i, f.Name, glm::vec3( c ) );
                        else
                            written = w.WriteColor( i, f.Name, c );
                    }
                    if ( !written )
                    {
                        LOG_ERROR( "[Lua] VFX.writeChannel '{}' entry {}: {}", name, i + 1, written.GetError() );
                        ok = false;
                    }
                }
            }
            return ok;
        };
    }
} // namespace Desert::Scripting

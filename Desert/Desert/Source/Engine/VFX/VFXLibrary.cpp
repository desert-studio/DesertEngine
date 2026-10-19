#include <Engine/VFX/VFXLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXUserParams.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <glm/glm.hpp>

#include <optional>

namespace Desert::VFX
{
    namespace
    {
        using Reflection::Value;

        std::optional<double> Number( const Value& value )
        {
            switch ( value.Type() )
            {
                case Reflection::FieldType::Double:
                    return *value.Get<double>();
                case Reflection::FieldType::Float:
                    return static_cast<double>( *value.Get<float>() );
                case Reflection::FieldType::Int:
                    return static_cast<double>( *value.Get<std::int64_t>() );
                case Reflection::FieldType::UInt:
                    return static_cast<double>( *value.Get<std::uint64_t>() );
                default:
                    return std::nullopt;
            }
        }

        // A vector, a colour or a list of up to four numbers as x, y, z, w (absent components 0).
        std::optional<glm::vec4> Components( const Value& value )
        {
            if ( const auto* v = value.Get<Value::Float3>() )
                return glm::vec4( ( *v )[0], ( *v )[1], ( *v )[2], 0.0f );
            if ( const auto* v = value.Get<Value::Float4>() )
                return glm::vec4( ( *v )[0], ( *v )[1], ( *v )[2], ( *v )[3] );
            const auto* list = value.Get<Value::Array>();
            if ( list == nullptr )
                return std::nullopt;
            glm::vec4 c( 0.0f );
            for ( std::size_t i = 0; i < list->size() && i < 4; ++i )
            {
                const std::optional<double> n = Number( ( *list )[i] );
                if ( !n )
                    return std::nullopt;
                c[static_cast<glm::length_t>( i )] = static_cast<float>( *n );
            }
            return c;
        }

        Common::BoolResultStr WriteField( VFXDataChannelWriter& w, std::size_t index,
                                          const Assets::Serialization::VFXDataChannelField& f, const Value& value )
        {
            using T = Assets::Serialization::VFXDataChannelFieldType;
            if ( f.Type == T::Float || f.Type == T::Int )
            {
                const std::optional<double> n = Number( value );
                if ( !n )
                    return Common::MakeFormattedError<bool>( "field '{}' needs a number", f.Name );
                return f.Type == T::Float ? w.WriteFloat( index, f.Name, static_cast<float>( *n ) )
                                          : w.WriteInt( index, f.Name, static_cast<std::int32_t>( *n ) );
            }
            const std::optional<glm::vec4> c = Components( value );
            if ( !c )
                return Common::MakeFormattedError<bool>( "field '{}' needs a vector or a list of numbers",
                                                         f.Name );
            if ( f.Type == T::Position )
                return w.WritePosition( index, f.Name, glm::vec3( *c ) );
            if ( f.Type == T::Direction )
                return w.WriteDirection( index, f.Name, glm::vec3( *c ) );
            return w.WriteColor( index, f.Name, *c );
        }
    } // namespace

    // The name is resolved by the scene's AssetManager (VFXDataChannels::Use), never by reading the file here: the
    // manager is the one place assets are loaded, so a channel the editor holds (any folder) is the one a script
    // gets, and an unknown name is refused by name.
    bool VFXLibrary::UseChannel( const std::string& name )
    {
        const Core::WorldContext* world = Core::WorldContext::Current();
        if ( world == nullptr || world->World == nullptr )
            return false;
        if ( world->Assets == nullptr )
        {
            LOG_ERROR( "[Script] VFX.useChannel('{}'): no AssetManager bound to resolve the channel through",
                       name );
            return false;
        }
        const Common::BoolResultStr used =
             world->World->GetVFXWorld().GetDataChannels().Use( name, *world->Assets );
        if ( !used )
            LOG_ERROR( "[Script] VFX.useChannel('{}'): {}", name, used.GetError() );
        return used.IsSuccess();
    }

    bool VFXLibrary::WriteChannel( const std::string& name, const std::vector<Reflection::Value::Map>& entries )
    {
        const Core::WorldContext* world = Core::WorldContext::Current();
        if ( world == nullptr || world->World == nullptr )
            return false;
        VFXDataChannels&      channels = world->World->GetVFXWorld().GetDataChannels();
        const VFXDataChannel* channel  = channels.Find( name );
        if ( channel == nullptr )
        {
            LOG_ERROR( "[Script] VFX.writeChannel: no data channel '{}' (VFX.useChannel first)", name );
            return false;
        }
        const auto fields = channel->Layout().Fields; // the writer below appends to the same channel
        auto       writer = channels.Write( name, entries.size() );
        if ( !writer.IsSuccess() )
        {
            LOG_ERROR( "[Script] VFX.writeChannel: {}", writer.GetError() );
            return false;
        }
        VFXDataChannelWriter w  = writer.ExtractValue();
        bool                 ok = true;
        for ( std::size_t i = 0; i < entries.size(); ++i )
            for ( const auto& f : fields )
            {
                const Value* value = entries[i].Find( f.Name );
                if ( value == nullptr )
                    continue; // an unwritten field stays zero
                if ( Common::BoolResultStr const written = WriteField( w, i, f, *value ); !written )
                {
                    LOG_ERROR( "[Script] VFX.writeChannel '{}' entry {}: {}", name, i + 1, written.GetError() );
                    ok = false;
                }
            }
        return ok;
    }

    bool VFXLibrary::SetParam( ECS::Entity entity, const std::string& name, const glm::vec4& value )
    {
        if ( !entity || !entity.HasComponent<ECS::VFXComponent>() )
        {
            LOG_WARN( "[VFX] setParam('{}'): the entity has no VFX component", name );
            return false;
        }
        const auto set = SetComponentUserParam( entity.GetComponent<ECS::VFXComponent>().Data, name, value );
        if ( !set )
            LOG_WARN( "[VFX] setParam: {}", set.GetError() );
        return set.IsSuccess();
    }

    bool VFXLibrary::ClearParam( ECS::Entity entity, const std::string& name )
    {
        if ( !entity || !entity.HasComponent<ECS::VFXComponent>() )
        {
            LOG_WARN( "[VFX] clearParam('{}'): the entity has no VFX component", name );
            return false;
        }
        return ClearUserParamOverride( entity.GetComponent<ECS::VFXComponent>().Data.UserParams, name );
    }
} // namespace Desert::VFX

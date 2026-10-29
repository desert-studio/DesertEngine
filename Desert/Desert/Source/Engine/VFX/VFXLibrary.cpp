#include <Engine/VFX/VFXLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/VFX/VFXSystemService.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <glm/glm.hpp>

#include <algorithm>
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

        // @p value packed for a parameter of @p type (the type's components first, the rest 0), or why it does not
        // fit.
        Common::ResultStr<glm::vec4> PackUserValue( Assets::Serialization::VFXValueType type, const Value& value )
        {
            using T = Assets::Serialization::VFXValueType;
            if ( type == T::Bool )
                if ( const auto* b = value.Get<bool>() )
                    return Common::MakeSuccess( glm::vec4( *b ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f ) );
            if ( type == T::Float || type == T::Int || type == T::Bool )
            {
                const std::optional<double> n = Number( value );
                if ( !n )
                    return Common::MakeError<glm::vec4>( "needs a number" );
                float x = static_cast<float>( *n );
                if ( type == T::Int )
                    x = static_cast<float>( static_cast<std::int32_t>( *n ) );
                if ( type == T::Bool )
                    x = *n != 0.0 ? 1.0f : 0.0f;
                return Common::MakeSuccess( glm::vec4( x, 0.0f, 0.0f, 0.0f ) );
            }
            const std::optional<glm::vec4> c = Components( value );
            if ( !c )
                return Common::MakeError<glm::vec4>( "needs a vector or a list of numbers" );
            glm::vec4      packed( 0.0f );
            const uint32_t count = Assets::Serialization::ComponentCount( type );
            for ( uint32_t i = 0; i < count; ++i )
                packed[static_cast<glm::length_t>( i )] = ( *c )[static_cast<glm::length_t>( i )];
            return Common::MakeSuccess( std::move( packed ) );
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

    // The type check reads the system's row, so a value of the wrong shape is refused here, by name, instead of
    // being packed into components the parameter never reads.
    bool VFXLibrary::SetVariable( ECS::Entity entity, const std::string& name, const Reflection::Value& value )
    {
        if ( !entity || !entity.HasComponent<ECS::VFXComponent>() )
        {
            LOG_ERROR( "[Script] VFX.setVariable('{}'): the entity has no VFX component", name );
            return false;
        }
        auto& data    = entity.GetComponent<ECS::VFXComponent>().Data;
        auto* service = Runtime::ResourceRegistry::GetVFXSystemService();
        if ( service == nullptr )
            return false;
        const auto system = service->Get( data.System );
        if ( !system || !system.GetValue() )
        {
            LOG_ERROR( "[Script] VFX.setVariable('{}'): the system is {}", name,
                       system ? std::string( "still loading" ) : system.GetError() );
            return false;
        }
        const auto& rows = system.GetValue()->UserParams;
        const auto row = std::find_if( rows.begin(), rows.end(), [&]( const auto& r ) { return r.Name == name; } );
        if ( row == rows.end() )
        {
            LOG_ERROR( "[Script] VFX.setVariable: the system has no User parameter '{}'", name );
            return false;
        }
        const auto packed = PackUserValue( row->Type, value );
        if ( !packed )
        {
            LOG_ERROR( "[Script] VFX.setVariable('{}'): the parameter {}", name, packed.GetError() );
            return false;
        }
        data.SetVariable( name, packed.GetValue() );
        return true;
    }
} // namespace Desert::VFX

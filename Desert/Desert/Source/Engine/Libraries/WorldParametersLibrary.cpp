#include <Engine/Libraries/WorldParametersLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>

namespace Desert::Libraries
{
    namespace
    {
        // The running script's world with its collection adopted: a script's OnStart runs before the scene's first
        // OnUpdate has synced, and must find the parameters all the same.
        Common::ResultStr<Core::Scene*> SyncedWorld( const char* call )
        {
            auto world = ScriptWorld( "WorldParameters", call );
            if ( !world )
                return world;
            if ( const auto synced = world.GetValue()->SyncWorldParameters(); !synced )
                return Common::MakeFormattedError<Core::Scene*>( "WorldParameters.{}: {}", call,
                                                                 synced.GetError() );
            return world;
        }
    } // namespace

    Common::BoolResultStr WorldParametersLibrary::SetScalar( const std::string& name, const float value )
    {
        const auto world = SyncedWorld( "setScalar" );
        if ( !world )
            return Common::MakeError( world.GetError() );
        return world.GetValue()->GetWorldParameters().SetScalar( name, value );
    }

    Common::BoolResultStr WorldParametersLibrary::SetVector( const std::string& name, const glm::vec4& value )
    {
        const auto world = SyncedWorld( "setVector" );
        if ( !world )
            return Common::MakeError( world.GetError() );
        return world.GetValue()->GetWorldParameters().SetVector( name, value );
    }

    Common::ResultStr<float> WorldParametersLibrary::GetScalar( const std::string& name )
    {
        const auto world = SyncedWorld( "getScalar" );
        if ( !world )
            return Common::MakeError<float>( world.GetError() );
        return world.GetValue()->GetWorldParameters().GetScalar( name );
    }

    Common::ResultStr<glm::vec4> WorldParametersLibrary::GetVector( const std::string& name )
    {
        const auto world = SyncedWorld( "getVector" );
        if ( !world )
            return Common::MakeError<glm::vec4>( world.GetError() );
        return world.GetValue()->GetWorldParameters().GetVector( name );
    }
} // namespace Desert::Libraries

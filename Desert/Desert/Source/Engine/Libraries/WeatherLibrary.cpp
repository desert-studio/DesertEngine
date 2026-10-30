#include <Engine/Libraries/WeatherLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Libraries
{
    Common::BoolResultStr WeatherLibrary::SetWeather( ECS::Entity zone, const std::string& preset,
                                                      const float seconds )
    {
        const auto world = ScriptWorld( "Weather", "set" );
        if ( !world )
            return Common::MakeError( world.GetError() );
        const auto handle = Runtime::ResourceRegistry::GetWeatherPresetService()->HandleForPath( preset );
        if ( !handle )
            return Common::MakeFormattedError<bool>( "Weather.set: {}", handle.GetError() );
        Core::Scene* scene = world.GetValue();
        if ( auto set =
                  scene->GetWeather().Set( scene->GetRegistry(), zone.GetHandle(), handle.GetValue(), seconds );
             !set )
            return Common::MakeFormattedError<bool>( "Weather.set: {}", set.GetError() );
        return BOOLSUCCESS;
    }

    Common::ResultStr<float> WeatherLibrary::GetScalarAt( const glm::vec3& point, const std::string& name )
    {
        const auto world = ScriptWorld( "Weather", "getScalar" );
        if ( !world )
            return Common::MakeError<float>( world.GetError() );
        Core::Scene*              scene = world.GetValue();
        const Core::WeatherValues here  = scene->GetWeather().At( scene->GetRegistry(), point );
        const auto                given = [&name]( const std::optional<float>& v ) -> Common::ResultStr<float>
        {
            if ( !v )
                return Common::MakeFormattedError<float>( "Weather.getScalar: no zone states '{}' here", name );
            return Common::MakeSuccess( *v );
        };
        if ( name == "CloudCoverage" )
            return given( here.CloudCoverage );
        if ( name == "FogDensity" )
            return given( here.FogDensity );
        if ( name == "WindSpeed" )
            return given( here.WindSpeed );
        if ( const auto* p = here.Find( name ) )
            return Common::MakeSuccess( p->Value[0] );
        return Common::MakeFormattedError<float>( "Weather.getScalar: no zone states '{}' here", name );
    }
} // namespace Desert::Libraries

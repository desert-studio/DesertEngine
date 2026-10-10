#include <Engine/Libraries/GameLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Audio/AudioEngine.hpp>
#include <Engine/Core/LevelTravel.hpp>
#include <Engine/Project/GameSettings.hpp>
#include <Engine/Project/ProjectContext.hpp>

namespace Desert::Libraries
{
    void LogLibrary::Info( const std::string& message )
    {
        LOG_INFO( "[Script] {}", message );
    }

    void LogLibrary::Warn( const std::string& message )
    {
        LOG_WARN( "[Script] {}", message );
    }

    void LogLibrary::Error( const std::string& message )
    {
        LOG_ERROR( "[Script] {}", message );
    }

    void AudioLibrary::Play( const std::string& clip, float volume )
    {
        Audio::AudioEngine::Get().PlayOneShot( clip, volume );
    }

    void AudioLibrary::StopAll()
    {
        Audio::AudioEngine::Get().StopAll();
    }

    std::string ProjectLibrary::Name()
    {
        return Project::ProjectContext::HasProject() ? Project::ProjectContext::Current().Name : std::string();
    }

    std::string ProjectLibrary::Company()
    {
        return Project::CurrentGameSettings().Company;
    }

    bool LevelLibrary::Open( const std::string& level )
    {
        const Common::BoolResultStr opened = Core::OpenLevel( level );
        if ( !opened )
            LOG_ERROR( "[Script] level.open('{}'): {}", level, opened.GetError() );
        return static_cast<bool>( opened );
    }
} // namespace Desert::Libraries

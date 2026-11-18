#include <Engine/Libraries/GameLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Audio/AudioEngine.hpp>
#include <Engine/Core/LevelTravel.hpp>
#include <Engine/Core/TimerManager.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Projects/GameSettings.hpp>
#include <Projects/ProjectContext.hpp>

#include <string_view>

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
        // A path names no `.desound`, so it states no class or loading: Effects, decoded whole (AUD-API moves
        // Audio.play to a sound GUID and takes both from the asset).
        Audio::AudioEngine::Get().PlayOneShot(
             Audio::SoundSource{ clip, Audio::SoundClass::Effects, Audio::SoundLoading::Resident }, volume );
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

    Common::BoolResultStr LevelLibrary::Open( const std::string& level )
    {
        // Core::OpenLevel logs its refusal; the script hears it as (false, reason).
        return Core::OpenLevel( level );
    }

    namespace
    {
        /// The running script's world timers; nullptr, logged, outside a world with timers (no Play).
        Core::TimerManager* WorldTimers( std::string_view function )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->Timers == nullptr )
            {
                LOG_ERROR( "[Script] Timer.{}: no world with timers runs this call", function );
                return nullptr;
            }
            return context->Timers;
        }
    } // namespace

    Core::TimerHandle TimerLibrary::After( float seconds, const Reflection::Callable& callback )
    {
        Core::TimerManager* timers = WorldTimers( "after" );
        if ( timers == nullptr )
            return Core::TimerHandle();
        return seconds > 0.0f ? timers->SetTimer( callback, seconds ) : timers->SetTimerForNextTick( callback );
    }

    Core::TimerHandle TimerLibrary::Every( float seconds, const Reflection::Callable& callback, float firstDelay,
                                           bool maxOncePerFrame )
    {
        Core::TimerManager* timers = WorldTimers( "every" );
        if ( timers == nullptr )
            return Core::TimerHandle();
        Core::TimerParameters parameters;
        parameters.Loop            = true;
        parameters.MaxOncePerFrame = maxOncePerFrame;
        parameters.FirstDelay      = firstDelay;
        const Core::TimerHandle handle = timers->SetTimer( callback, seconds, parameters );
        if ( handle.Serial == 0 && seconds <= 0.0f )
            LOG_ERROR( "[Script] Timer.every: a period of {} s sets nothing (it must be above 0)", seconds );
        return handle;
    }

    Core::TimerHandle TimerLibrary::NextTick( const Reflection::Callable& callback )
    {
        Core::TimerManager* timers = WorldTimers( "nextTick" );
        return timers != nullptr ? timers->SetTimerForNextTick( callback ) : Core::TimerHandle();
    }

    void TimerLibrary::Clear( const Core::TimerHandle& handle )
    {
        if ( Core::TimerManager* timers = WorldTimers( "clear" ) )
            timers->ClearTimer( handle );
    }

    void TimerLibrary::Pause( const Core::TimerHandle& handle )
    {
        if ( Core::TimerManager* timers = WorldTimers( "pause" ) )
            timers->PauseTimer( handle );
    }

    void TimerLibrary::UnPause( const Core::TimerHandle& handle )
    {
        if ( Core::TimerManager* timers = WorldTimers( "unpause" ) )
            timers->UnPauseTimer( handle );
    }

    bool TimerLibrary::IsActive( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "isActive" );
        return timers != nullptr && timers->IsTimerActive( handle );
    }

    bool TimerLibrary::IsPaused( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "isPaused" );
        return timers != nullptr && timers->IsTimerPaused( handle );
    }

    bool TimerLibrary::IsPending( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "isPending" );
        return timers != nullptr && timers->IsTimerPending( handle );
    }

    bool TimerLibrary::Exists( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "exists" );
        return timers != nullptr && timers->TimerExists( handle );
    }

    float TimerLibrary::Rate( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "rate" );
        return timers != nullptr ? timers->GetTimerRate( handle ) : -1.0f;
    }

    float TimerLibrary::Elapsed( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "elapsed" );
        return timers != nullptr ? timers->GetTimerElapsed( handle ) : -1.0f;
    }

    float TimerLibrary::Remaining( const Core::TimerHandle& handle )
    {
        const Core::TimerManager* timers = WorldTimers( "remaining" );
        return timers != nullptr ? timers->GetTimerRemaining( handle ) : -1.0f;
    }
} // namespace Desert::Libraries

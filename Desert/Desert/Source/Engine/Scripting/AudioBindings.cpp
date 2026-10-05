#include "Internal/ScriptRuntime.hpp"

#include <Engine/Audio/AudioEngine.hpp>

namespace Desert::Scripting
{
    // Audio table: fire-and-forget playback for gameplay scripts.
    //   Audio.play("Sounds/shot.wav")          -- 2D one-shot, full volume
    //   Audio.play("Sounds/shot.wav", 0.5)     -- with volume
    //   Audio.stopAll()                        -- silence everything (managed sources restart via AutoPlay)
    // Paths are audio FILES (absolute or Assets-relative, VFS-aware), not Sound assets: scripting is deferred
    // by the owner, and Audio Source components name `.desound` assets instead.
    void RegisterAudioBindings( ScriptEngine::Impl& implRef )
    {
        sol::table audio = implRef.Lua.create_named_table( "Audio" );

        audio["play"] = sol::overload(
             []( const std::string& clip ) { Audio::AudioEngine::Get().PlayOneShot( clip ); },
             []( const std::string& clip, float volume )
             { Audio::AudioEngine::Get().PlayOneShot( clip, volume ); } );

        audio["stopAll"] = []() { Audio::AudioEngine::Get().StopAll(); };
    }
} // namespace Desert::Scripting

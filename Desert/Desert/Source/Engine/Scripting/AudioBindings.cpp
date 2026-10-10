#include "Internal/ScriptRuntime.hpp"

#include <Engine/Audio/AudioEngine.hpp>

namespace Desert::Scripting
{
    namespace
    {
        // Audio.play(clip [, volume]) — a one-shot.
        int Play( lua_State* L )
        {
            const std::string clip = luaL_checkstring( L, 1 );
            if ( lua_isnoneornil( L, 2 ) )
                Audio::AudioEngine::Get().PlayOneShot( clip, Audio::SoundClass::Effects );
            else
                Audio::AudioEngine::Get().PlayOneShot( clip, Audio::SoundClass::Effects, static_cast<float>( luaL_checknumber( L, 2 ) ) );
            return 0;
        }
        int StopAll( lua_State* )
        {
            Audio::AudioEngine::Get().StopAll();
            return 0;
        }
    } // namespace

    void RegisterAudioBindings( lua_State* L )
    {
        constexpr luaL_Reg kAudio[] = { { "play", &Play }, { "stopAll", &StopAll }, { nullptr, nullptr } };
        luaL_register( L, "Audio", kAudio );
        lua_pop( L, 1 );
    }
} // namespace Desert::Scripting

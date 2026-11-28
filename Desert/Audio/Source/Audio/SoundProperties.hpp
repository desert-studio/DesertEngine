#pragma once

#include <Audio/AudioMix.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Audio
{
    // HOW A SOUND'S PCM REACHES THE MIX — UE's USoundWave loading choice, stated by the `.desound`.
    //   Resident — decoded once into a buffer shared by every voice of that sound: short and played often.
    //   Streamed — decoded page by page while it plays, one decoder per voice: long and played once.
    enum class SoundLoading : uint8_t
    {
        Resident,
        Streamed,
    };

    [[nodiscard]] constexpr std::string_view SoundLoadingName( SoundLoading loading )
    {
        switch ( loading )
        {
            case SoundLoading::Resident:
                return "Resident";
            case SoundLoading::Streamed:
                return "Streamed";
        }
        return {};
    }

    [[nodiscard]] constexpr std::optional<SoundLoading> SoundLoadingNamed( std::string_view name )
    {
        for ( const SoundLoading loading : { SoundLoading::Resident, SoundLoading::Streamed } )
            if ( SoundLoadingName( loading ) == name )
                return loading;
        return std::nullopt;
    }

    // WHAT A VOICE PLAYS: the clip (a content key or an absolute path), the class asset whose group it plays in
    // and how its PCM is loaded. Every member is stated by the caller; a Class that AudioMix does not hold is
    // refused by AudioEngine with the GUID in the log — there is no default class.
    struct SoundSource
    {
        std::string  Clip;
        SoundClassId Class;
        SoundLoading Loading;
    };
} // namespace Desert::Audio

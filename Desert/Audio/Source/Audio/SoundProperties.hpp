#pragma once

#include <Audio/AudioMix.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Audio
{
    // HOW A SOUND'S PCM REACHES THE MIX — UE's USoundWave loading choice (ESoundWaveLoadingBehavior + the
    // UE4 `bStreaming` flag), stated by the `.desound` (Assets::SoundAsset), never guessed from a file size.
    //   Resident — decoded once into a buffer the resource manager shares between every voice of that sound
    //              (UE RetainOnLoad / ForceInline): footsteps, impacts, UI — short and played often.
    //   Streamed — decoded page by page while it plays, one decoder per voice (UE bStreaming / LoadOnDemand
    //              through the stream cache): music, ambience beds, dialogue — long and played once.
    // Both load on the resource manager's job thread (MA_SOUND_FLAG_ASYNC): a voice that starts before its
    // data is ready is silent until it is, and a seek asked before then is applied on its first read.
    enum class SoundLoading : uint8_t
    {
        Resident,
        Streamed,
    };

    // The names the `.desound` writes — the enumerators' own spelling, one table per enum.
    [[nodiscard]] constexpr std::string_view SoundClassName( SoundClass soundClass )
    {
        switch ( soundClass )
        {
            case SoundClass::Music:
                return "Music";
            case SoundClass::Effects:
                return "Effects";
            case SoundClass::Voice:
                return "Voice";
        }
        return {};
    }

    [[nodiscard]] constexpr std::optional<SoundClass> SoundClassNamed( std::string_view name )
    {
        for ( const SoundClass soundClass : { SoundClass::Music, SoundClass::Effects, SoundClass::Voice } )
            if ( SoundClassName( soundClass ) == name )
                return soundClass;
        return std::nullopt;
    }

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

    // WHAT A VOICE PLAYS: the clip (a content key "assets:Audio/Hit.wav" or an absolute path — what
    // Assets::SoundAsset::Resolve hands over), the class whose volume scales it and how its PCM is loaded.
    // Every member is stated by the caller; there is no default class and no default loading.
    struct SoundSource
    {
        std::string  Clip;
        SoundClass   Class;
        SoundLoading Loading;
    };
} // namespace Desert::Audio

#pragma once

#include <Engine/Media/MediaPlayer.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Desert::Media
{
    // What a project lists for its launch (UE Project Settings ▸ Movies: StartupMovies,
    // bMoviesAreSkippable, bWaitForMoviesToComplete). Paths are resolved by the host, in play order.
    struct StartupMovieSettings
    {
        std::vector<std::filesystem::path> Movies;
        bool                               Skippable         = true; // a key / click ends the current movie
        bool                               WaitForCompletion = true; // false: the game's readiness ends them
    };

    // The startup sequence (UE's FDefaultGameMoviePlayer): the listed movies play one after another, full
    // screen, while the game loads behind them, and the game is shown only once the sequence is over.
    //
    // This class is the SEQUENCE and the transport (one MediaPlayer reopened per movie); where the picture
    // and the sound go is the host's, exactly as VideoService splits it: the host owns a MediaTexture fed
    // from Player() and an IMediaAudioSink handed in here (null: the clock runs on Tick's delta).
    //
    // A movie that does not open or fails mid-play is reported through OnMovieFailed and the sequence goes
    // on with the next one: a missing intro is an authoring error the log names, not a reason to keep the
    // player from the game.
    class StartupMoviePlayer
    {
    public:
        explicit StartupMoviePlayer( StartupMovieSettings settings, IMediaAudioSink* sink = nullptr );
        StartupMoviePlayer( const StartupMoviePlayer& )            = delete; // OnEndReached captures `this`
        StartupMoviePlayer& operator=( const StartupMoviePlayer& ) = delete;

        // (movie path, the player's / demuxer's error). Set before Start.
        std::function<void( const std::filesystem::path&, const std::string& )> OnMovieFailed;

        // Opens the first movie that opens, ON ITS FIRST FRAME AND PAUSED: its clock starts at the first
        // NotifyFramePresented. A list with none that opens is Finished at once.
        void Start();

        // The host put the current movie's picture on the screen. A movie's clock (and its sound) starts on
        // the first of these after it opened, not at the open: a boot hitch or the first upload between the
        // two would otherwise be time the movie "played" unseen (UE's movie player starts on the shown frame).
        void NotifyFramePresented();

        // Forwarded to the player (MediaPlayer::SetBlockOnTime): an offline driver of the clock.
        void SetBlockOnTime( bool block )
        {
            m_Player.SetBlockOnTime( block );
        }

        // Once per frame: advances the current movie and moves on to the next when it ends.
        void Tick( double deltaSeconds );

        // The player pressed something. Ends the current movie (the next one starts) when the project made
        // the movies skippable; true when it did.
        bool Skip();

        // The game behind the movies has loaded. Without WaitForCompletion this ends the sequence now.
        void NotifyContentReady();

        bool Finished() const
        {
            return m_Index >= m_Settings.Movies.size();
        }
        // Index into the settings' list of the movie playing now; the list's size once Finished.
        std::size_t CurrentIndex() const
        {
            return m_Index;
        }
        const MediaPlayer& Player() const
        {
            return m_Player;
        }

    private:
        void OpenFrom( std::size_t index );

        StartupMovieSettings m_Settings;
        MediaPlayer          m_Player;
        std::size_t          m_Index = 0;
        bool                 m_Ended = false; // set by OnEndReached inside Tick, acted on after it
        bool                 m_AwaitingShown = false; // opened, paused until its picture is presented
    };
} // namespace Desert::Media

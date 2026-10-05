#include "StartupMoviePlayer.hpp"

#include <utility>

namespace Desert::Media
{
    StartupMoviePlayer::StartupMoviePlayer( StartupMovieSettings settings, IMediaAudioSink* sink )
         : m_Settings( std::move( settings ) )
    {
        m_Player.SetAudioSink( sink );
        m_Player.OnEndReached = [this] { m_Ended = true; };
    }

    void StartupMoviePlayer::Start()
    {
        OpenFrom( 0 );
    }

    void StartupMoviePlayer::OpenFrom( std::size_t index )
    {
        for ( ; index < m_Settings.Movies.size(); ++index )
        {
            const std::filesystem::path& movie = m_Settings.Movies[index];
            if ( const std::string error = m_Player.Open( MediaSource{ movie } ); !error.empty() )
            {
                if ( OnMovieFailed )
                    OnMovieFailed( movie, error );
                continue;
            }
            m_AwaitingShown = true; // Play comes with the first presented frame of it
            m_Index         = index;
            return;
        }
        m_Player.Close(); // the sound stops with the last movie
        m_AwaitingShown = false;
        m_Index         = m_Settings.Movies.size();
    }

    void StartupMoviePlayer::NotifyFramePresented()
    {
        if ( Finished() || !m_AwaitingShown )
            return;
        m_AwaitingShown = false;
        m_Player.Play();
        if ( OnMovieShown )
            OnMovieShown( m_Settings.Movies[m_Index], m_Index, m_Settings.Movies.size() );
    }

    void StartupMoviePlayer::Tick( double deltaSeconds )
    {
        if ( Finished() )
            return;
        // The next movie is opened HERE, after the player's Tick has returned, never from inside its
        // OnEndReached: reopening the player from within its own Tick would pull its state out from under it.
        m_Ended = false;
        m_Player.Tick( deltaSeconds );
        if ( m_Player.GetState() == MediaPlayerState::Error )
        {
            if ( OnMovieFailed )
                OnMovieFailed( m_Settings.Movies[m_Index], m_Player.Error() );
            OpenFrom( m_Index + 1 );
            return;
        }
        if ( m_Ended )
            OpenFrom( m_Index + 1 );
    }

    bool StartupMoviePlayer::Skip()
    {
        if ( !m_Settings.Skippable || Finished() )
            return false;
        OpenFrom( m_Index + 1 );
        return true;
    }

    void StartupMoviePlayer::NotifyContentReady()
    {
        if ( m_Settings.WaitForCompletion || Finished() )
            return;
        OpenFrom( m_Settings.Movies.size() );
    }
} // namespace Desert::Media
